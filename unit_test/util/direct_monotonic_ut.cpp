/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

module lucene_core:ut.direct_monotonic;

import lucene_core;

using namespace lucene;

namespace {

/// In-memory [`IndexOutput`] used as both the metadata and the data sink.
class VectorIndexOutput final : public IndexOutput {
public:
    [[nodiscard]] VoidResult write_byte(uint8_t b) override {
        bytes_.push_back(b);
        return {};
    }

    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override {
        if (offset + length > b.size()) {
            return err(LuceneError::illegal_argument("write out of bounds"));
        }
        bytes_.insert(bytes_.end(), b.begin() + static_cast<ptrdiff_t>(offset),
                      b.begin() + static_cast<ptrdiff_t>(offset + length));
        return {};
    }

    [[nodiscard]] Result<size_t> get_file_pointer() const override { return bytes_.size(); }
    [[nodiscard]] uint64_t get_checksum() override { return 0; }
    [[nodiscard]] std::string_view get_name() const override { return "memory"; }
    [[nodiscard]] VoidResult close() override { return {}; }

    [[nodiscard]] const std::vector<uint8_t> &bytes() const noexcept { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
};

/// Minimal [`RandomAccessInput`] over an in-memory buffer.
class ArrayRandomAccessInput final : public RandomAccessInput {
public:
    explicit ArrayRandomAccessInput(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] Result<uint8_t> read_byte(size_t pos) const override {
        if (pos >= bytes_.size()) {
            return err(LuceneError::io("read past end"));
        }
        return bytes_[pos];
    }

    [[nodiscard]] Result<int16_t> read_short(size_t pos) const override { return read_integer<int16_t>(pos, 2); }
    [[nodiscard]] Result<int32_t> read_int(size_t pos) const override { return read_integer<int32_t>(pos, 4); }
    [[nodiscard]] Result<int64_t> read_long(size_t pos) const override { return read_integer<int64_t>(pos, 8); }

    [[nodiscard]] VoidResult read_bytes(size_t pos, std::span<uint8_t> out) const override {
        if (pos + out.size() > bytes_.size()) {
            return err(LuceneError::io("read past end"));
        }
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = bytes_[pos + i];
        }
        return {};
    }

    [[nodiscard]] Result<size_t> length() const override { return bytes_.size(); }

private:
    template <class T>
    [[nodiscard]] Result<T> read_integer(size_t pos, size_t num_bytes) const {
        if (pos + num_bytes > bytes_.size()) {
            return err(LuceneError::io("read past end"));
        }
        uint64_t value = 0;
        for (size_t i = 0; i < num_bytes; ++i) {
            value |= static_cast<uint64_t>(bytes_[pos + i]) << (8 * i);
        }
        return static_cast<T>(value);
    }

    std::span<const uint8_t> bytes_;
};

} // namespace

TEST(DirectMonotonicTest, RoundTripsMonotonicValues) {
    constexpr int32_t kBlockShift = 4; // 16 values per block
    constexpr size_t kCount = 100;

    std::vector<int64_t> values;
    values.reserve(kCount);
    for (size_t i = 0; i < kCount; ++i) {
        // Strictly increasing and not exactly linear, so the block deltas are
        // non-zero and the "expected" slope is exercised.
        values.push_back(static_cast<int64_t>(i) * 3 + static_cast<int64_t>(i / 4));
    }

    VectorIndexOutput meta_out;
    VectorIndexOutput data_out;
    auto writer = DirectMonotonicWriter::get_instance(meta_out, data_out, static_cast<int64_t>(kCount), kBlockShift);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (const int64_t value : values) {
        auto added = writer->add(value);
        ASSERT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
    }
    ASSERT_TRUE(writer->finish().has_value());

    ByteArrayDataInput meta_in(meta_out.bytes());
    auto meta = DirectMonotonicMeta::load(meta_in, static_cast<int64_t>(kCount), kBlockShift);
    ASSERT_TRUE(meta.has_value()) << (meta ? "" : meta.error().to_string());
    EXPECT_EQ(meta->num_blocks(), (kCount + 15) / 16);
    EXPECT_EQ(meta->block_shift, kBlockShift);

    const ArrayRandomAccessInput data(data_out.bytes());
    const DirectMonotonicReader reader(*meta, data);
    for (size_t i = 0; i < kCount; ++i) {
        auto value = reader.get(i);
        ASSERT_TRUE(value.has_value()) << (value ? "" : value.error().to_string());
        EXPECT_EQ(value.value(), values[i]) << "index " << i;
    }

    // Every stored value must be findable, and absent keys resolve to
    // `-1 - insertion_point`.
    for (size_t i = 0; i < kCount; ++i) {
        auto found = reader.binary_search(0, static_cast<int64_t>(kCount), values[i]);
        ASSERT_TRUE(found.has_value()) << (found ? "" : found.error().to_string());
        EXPECT_EQ(found.value(), static_cast<int64_t>(i));
    }
    // `values[1] - 1` lies between values[0] and values[1], so insertion point 1.
    auto missing = reader.binary_search(0, static_cast<int64_t>(kCount), values[1] - 1);
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing.value(), -2);
}

TEST(DirectMonotonicTest, ConstantBlocksStoreNoData) {
    constexpr int32_t kBlockShift = 2; // 4 values per block
    constexpr size_t kCount = 10;

    VectorIndexOutput meta_out;
    VectorIndexOutput data_out;
    auto writer = DirectMonotonicWriter::get_instance(meta_out, data_out, static_cast<int64_t>(kCount), kBlockShift);
    ASSERT_TRUE(writer.has_value());
    for (size_t i = 0; i < kCount; ++i) {
        ASSERT_TRUE(writer->add(42).has_value());
    }
    ASSERT_TRUE(writer->finish().has_value());
    EXPECT_TRUE(data_out.bytes().empty()) << "a constant sequence needs no data bytes";

    ByteArrayDataInput meta_in(meta_out.bytes());
    auto meta = DirectMonotonicMeta::load(meta_in, static_cast<int64_t>(kCount), kBlockShift);
    ASSERT_TRUE(meta.has_value());
    for (const int32_t bpv : meta->bpvs) {
        EXPECT_EQ(bpv, 0);
    }

    const ArrayRandomAccessInput data(data_out.bytes());
    const DirectMonotonicReader reader(*meta, data);
    for (size_t i = 0; i < kCount; ++i) {
        auto value = reader.get(i);
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(value.value(), 42);
    }
}

TEST(DirectMonotonicTest, ValidatesInput) {
    VectorIndexOutput meta_out;
    VectorIndexOutput data_out;

    auto bad_shift = DirectMonotonicWriter::get_instance(meta_out, data_out, 10, MIN_BLOCK_SHIFT - 1);
    ASSERT_FALSE(bad_shift.has_value());
    EXPECT_EQ(bad_shift.error().kind(), ErrorKind::kIllegalArgument);

    auto negative_count = DirectMonotonicWriter::get_instance(meta_out, data_out, -1, 4);
    ASSERT_FALSE(negative_count.has_value());

    auto writer = DirectMonotonicWriter::get_instance(meta_out, data_out, 3, 4);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->add(10).has_value());
    auto unordered = writer->add(9);
    ASSERT_FALSE(unordered.has_value());
    EXPECT_EQ(unordered.error().kind(), ErrorKind::kIllegalArgument);

    // Finishing with the wrong number of values fails.
    auto too_early = writer->finish();
    ASSERT_FALSE(too_early.has_value());
    EXPECT_EQ(too_early.error().kind(), ErrorKind::kIllegalState);
}

TEST(DirectMonotonicTest, HandlesEmptySequence) {
    VectorIndexOutput meta_out;
    VectorIndexOutput data_out;
    auto writer = DirectMonotonicWriter::get_instance(meta_out, data_out, 0, 4);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->finish().has_value());
    EXPECT_TRUE(meta_out.bytes().empty());

    ByteArrayDataInput meta_in(meta_out.bytes());
    auto meta = DirectMonotonicMeta::load(meta_in, 0, 4);
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->num_blocks(), 0u);

    const ArrayRandomAccessInput data(data_out.bytes());
    const DirectMonotonicReader reader(*meta, data);
    EXPECT_FALSE(reader.get(0).has_value());
}
