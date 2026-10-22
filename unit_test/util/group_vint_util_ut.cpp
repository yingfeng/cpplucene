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

module lucene_core:ut.group_vint_util;

import lucene_core;

using namespace lucene;

namespace {

/// A `DataInput` that also exposes random access reads, mirroring the
/// `DI: DataInput + IntReader` trait bound of the reference implementation.
class SpanIntReaderInput : public DataInput, public IntReader {
public:
    explicit SpanIntReaderInput(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] Result<uint8_t> read_byte() override {
        if (pos_ >= bytes_.size()) {
            return err(LuceneError::eof("read past end"));
        }
        return bytes_[pos_++];
    }

    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override {
        if (pos_ + b.size() > bytes_.size()) {
            return err(LuceneError::eof("read past end"));
        }
        std::ranges::copy(bytes_.subspan(pos_, b.size()), b.begin());
        pos_ += b.size();
        return {};
    }

    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override {
        if (num_bytes < 0 || pos_ + static_cast<size_t>(num_bytes) > bytes_.size()) {
            return err(LuceneError::eof("skip past end"));
        }
        pos_ += static_cast<size_t>(num_bytes);
        return {};
    }

    [[nodiscard]] Result<int32_t> read(size_t pos) override {
        if (pos + 4 > bytes_.size()) {
            return err(LuceneError::eof("read past end"));
        }
        return BitUtil::get_i32_le(bytes_, pos);
    }

private:
    std::span<const uint8_t> bytes_;
    size_t pos_{0};
};

constexpr std::array<int32_t, 9> kValues = {0, 1, 0xFF, 0x100, 0xFFFF, 0x10000, 0xFFFFFF, 0x1000000, -1};

/// Encodes `values[0..limit)` with the group-varint writer.
std::vector<uint8_t> encode_i32(std::span<const int32_t> values) {
    std::vector<uint8_t> bytes(values.size() * GroupVIntUtil::MAX_LENGTH_PER_GROUP + 8);
    ByteArrayDataOutput out(bytes);
    std::array<uint8_t, GroupVIntUtil::MAX_LENGTH_PER_GROUP> scratch{};
    auto result = GroupVIntUtil::write_group_vints_i32(out, scratch, values, values.size());
    EXPECT_TRUE(result.has_value());
    bytes.resize(out.get_position());
    return bytes;
}

} // namespace

TEST(GroupVIntUtilTest, LengthPerGroupDependsOnMagnitude) {
    // 4 values of one byte each: flag + 4 bytes.
    const std::array<int32_t, 4> small = {0, 1, 2, 3};
    EXPECT_EQ(encode_i32(small).size(), 5u);

    // 4 values of four bytes each: flag + 16 bytes.
    const std::array<int32_t, 4> large = {-1, -1, -1, -1};
    EXPECT_EQ(encode_i32(large).size(), 17u);
    EXPECT_EQ(GroupVIntUtil::MAX_LENGTH_PER_GROUP, 17u);

    // Flag byte of {0, 1, 2, 3} encodes (1, 1, 1, 1) byte widths.
    EXPECT_EQ(encode_i32(small)[0], 0x00);
    // Flag byte of {0xFF, 0x100, 0x10000, 0x1000000} encodes widths (1, 2, 3, 4).
    const std::array<int32_t, 4> widths = {0xFF, 0x100, 0x10000, 0x1000000};
    EXPECT_EQ(encode_i32(widths)[0], 0x1B);
}

TEST(GroupVIntUtilTest, RoundTripI32WithTailVints) {
    const std::vector<uint8_t> bytes = encode_i32(kValues);
    // Two full groups plus one tail vint.
    std::array<int32_t, 9> decoded{};
    ByteArrayDataInput in(bytes);
    auto result = GroupVIntUtil::read_group_vints_i32(in, decoded, kValues.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(decoded, kValues);
    EXPECT_TRUE(in.eof());
}

TEST(GroupVIntUtilTest, RoundTripI64WithTailVints) {
    std::vector<int64_t> values(kValues.size());
    for (size_t i = 0; i < kValues.size(); ++i) {
        // Negative int32_t bit patterns surface as their unsigned value.
        values[i] = static_cast<int64_t>(static_cast<uint32_t>(kValues[i]));
    }

    std::vector<uint8_t> bytes(values.size() * GroupVIntUtil::MAX_LENGTH_PER_GROUP + 8);
    ByteArrayDataOutput out(bytes);
    std::array<uint8_t, GroupVIntUtil::MAX_LENGTH_PER_GROUP> scratch{};
    ASSERT_TRUE(GroupVIntUtil::write_group_vints_i64(out, scratch, values, values.size()).has_value());
    bytes.resize(out.get_position());

    std::vector<int64_t> decoded(values.size());
    ByteArrayDataInput in(bytes);
    ASSERT_TRUE(GroupVIntUtil::read_group_vints_i64(in, decoded, values.size()).has_value());
    EXPECT_EQ(decoded, values);
    EXPECT_TRUE(in.eof());
}

TEST(GroupVIntUtilTest, SingleGroupMatchesBulkDecode) {
    const std::array<int32_t, 4> group = {7, 300, 70000, 0x12345678};

    const std::vector<uint8_t> bytes = encode_i32(group);
    std::array<int32_t, 4> decoded{};
    ByteArrayDataInput in(bytes);
    ASSERT_TRUE(GroupVIntUtil::read_group_vint_i32(in, decoded, 0).has_value());
    EXPECT_EQ(decoded, group);

    std::array<int64_t, 4> decoded64{};
    ByteArrayDataInput in64(bytes);
    ASSERT_TRUE(GroupVIntUtil::read_group_vint_i64(in64, decoded64, 0).has_value());
    for (size_t i = 0; i < group.size(); ++i) {
        EXPECT_EQ(decoded64[i], static_cast<int64_t>(static_cast<uint32_t>(group[i])));
    }
}

TEST(GroupVIntUtilTest, WithReaderFastPathReturnsBytesConsumed) {
    const std::array<int32_t, 4> group = {0xFF, 0x100, 0x10000, 0x1000000};
    const std::vector<uint8_t> bytes = encode_i32(group);
    ASSERT_EQ(bytes.size(), 11u); // 1 flag + 1 + 2 + 3 + 4

    SpanIntReaderInput in(bytes);
    std::array<int64_t, 4> decoded{};
    // `pos` is the position *of* the flag byte: the fast path adds 1 for it.
    auto result = GroupVIntUtil::read_group_vint_i64_with_reader(
            in, in, GroupVIntUtil::MAX_LENGTH_PER_GROUP, 0, decoded, 0);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_EQ(result.value(), bytes.size() - 1);
    for (size_t i = 0; i < group.size(); ++i) {
        EXPECT_EQ(decoded[i], static_cast<int64_t>(static_cast<uint32_t>(group[i])));
    }

    SpanIntReaderInput in32(bytes);
    std::array<int32_t, 4> decoded32{};
    auto result32 = GroupVIntUtil::read_group_vint_i32_with_reader(
            in32, in32, GroupVIntUtil::MAX_LENGTH_PER_GROUP, 0, decoded32, 0);
    ASSERT_TRUE(result32.has_value());
    EXPECT_EQ(result32.value(), bytes.size() - 1);
    EXPECT_EQ(decoded32, group);

    // Not enough remaining bytes: falls back to the sequential decoder and
    // reports 0 bytes consumed.
    SpanIntReaderInput fallback(bytes);
    std::array<int32_t, 4> fallback32{};
    auto resultFallback = GroupVIntUtil::read_group_vint_i32_with_reader(fallback, fallback, 4, 0, fallback32, 0);
    ASSERT_TRUE(resultFallback.has_value());
    EXPECT_EQ(resultFallback.value(), 0u);
    EXPECT_EQ(fallback32, group);
}

TEST(GroupVIntUtilTest, WriteRejectsWideI64Values) {
    std::vector<int64_t> values = {0, 0, 0, 0x1'0000'0000ll};
    std::vector<uint8_t> bytes(64);
    ByteArrayDataOutput out(bytes);
    std::array<uint8_t, GroupVIntUtil::MAX_LENGTH_PER_GROUP> scratch{};
    auto result = GroupVIntUtil::write_group_vints_i64(out, scratch, values, values.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind(), ErrorKind::kNumberOverflow);
}

TEST(GroupVIntUtilTest, ReadRejectsShortDestination) {
    const std::array<int32_t, 4> group = {0, 0, 0, 0};
    const std::vector<uint8_t> bytes = encode_i32(group);

    std::array<int32_t, 3> dst{};
    ByteArrayDataInput in(bytes);
    auto result = GroupVIntUtil::read_group_vints_i32(in, dst, 4);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind(), ErrorKind::kIllegalArgument);
}
