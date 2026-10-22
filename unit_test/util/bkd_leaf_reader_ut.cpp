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
#include <memory>
#include <span>
#include <vector>

module lucene_core:ut.bkd_leaf_reader;

import lucene_core;

using namespace lucene;

namespace {

BKDConfig config_of(size_t dims, size_t index_dims, size_t max_points_in_leaf = 512) {
    auto config = BKDConfig::create(dims, index_dims, 4, max_points_in_leaf);
    EXPECT_TRUE(config.has_value());
    return config.value();
}

std::vector<uint8_t> pack1(int32_t dim0) {
    std::vector<uint8_t> packed(4, 0);
    BitUtil::set_i32_be(packed, 0, dim0);
    return packed;
}

std::vector<uint8_t> pack2(int32_t dim0, int32_t dim1) {
    std::vector<uint8_t> packed(8, 0);
    BitUtil::set_i32_be(packed, 0, dim0);
    BitUtil::set_i32_be(packed, 4, dim1);
    return packed;
}

/// An [`IndexInput`] over a borrowed byte buffer.
class MemoryIndexInput final : public IndexInput {
public:
    explicit MemoryIndexInput(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] Result<size_t> get_file_pointer() const override { return pos_; }
    [[nodiscard]] VoidResult seek(size_t pos) override {
        if (pos > bytes_.size()) {
            return err(LuceneError::eof("cannot seek past the end of the buffer"));
        }
        pos_ = pos;
        return {};
    }
    [[nodiscard]] Result<size_t> length() const override { return bytes_.size(); }
    [[nodiscard]] Result<uint8_t> read_byte() override {
        if (pos_ >= bytes_.size()) {
            return err(LuceneError::eof("read past the end of the buffer"));
        }
        return bytes_[pos_++];
    }
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override {
        if (pos_ + b.size() > bytes_.size()) {
            return err(LuceneError::eof("read past the end of the buffer"));
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(pos_), b.size(), b.begin());
        pos_ += b.size();
        return {};
    }
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> clone() const override {
        auto copy = std::make_unique<MemoryIndexInput>(bytes_);
        copy->pos_ = pos_;
        return std::unique_ptr<IndexInput>(std::move(copy));
    }

private:
    std::span<const uint8_t> bytes_;
    size_t pos_{0};
};

/// Writes one leaf block for the given points and returns the encoded bytes.
std::vector<uint8_t> write_block(const BKDConfig &config, const std::vector<std::vector<uint8_t>> &points) {
    HeapPointWriter writer(config, points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        EXPECT_TRUE(writer.append_bytes(points[i], static_cast<int32_t>(i)).has_value());
    }
    EXPECT_TRUE(writer.close().has_value());

    GrowableByteArrayDataOutput out;
    auto info = BkdLeafWriter::write_leaf_block(writer, BkdLeaf{0, points.size()}, out);
    EXPECT_TRUE(info.has_value());
    return out.take_bytes();
}

} // namespace

TEST(BkdLeafReaderTest, RoundTripsDistinctValues) {
    const BKDConfig config = config_of(1, 1);
    const std::vector<int32_t> dim0{0, 1, 2, 7, 7, 9};
    std::vector<std::vector<uint8_t>> points;
    for (const int32_t value : dim0) {
        points.push_back(pack1(value));
    }
    const auto block = write_block(config, points);

    MemoryIndexInput in(block);
    auto leaf = BkdLeafReader::read_leaf_block(in, config, 0);
    ASSERT_TRUE(leaf.has_value()) << (leaf ? "" : leaf.error().to_string());

    EXPECT_FALSE(leaf->all_equal);
    EXPECT_EQ(leaf->cardinality, 5u);
    ASSERT_EQ(leaf->size(), dim0.size());
    EXPECT_EQ(leaf->common_prefix_lengths.size(), 1u);
    EXPECT_EQ(leaf->common_prefix_lengths[0], 3u);
    for (size_t i = 0; i < dim0.size(); ++i) {
        EXPECT_EQ(leaf->doc_ids[i], static_cast<int32_t>(i));
        EXPECT_EQ(BitUtil::get_i32_be(leaf->values[i], 0), dim0[i]);
    }
    // The reader stops exactly at the end of the block.
    EXPECT_EQ(in.get_file_pointer().value(), block.size());
}

TEST(BkdLeafReaderTest, RoundTripsAllEqualBlocks) {
    const BKDConfig config = config_of(1, 1);
    std::vector<std::vector<uint8_t>> points(5, pack1(42));
    const auto block = write_block(config, points);

    MemoryIndexInput in(block);
    auto leaf = BkdLeafReader::read_leaf_block(in, config, 0);
    ASSERT_TRUE(leaf.has_value()) << (leaf ? "" : leaf.error().to_string());

    EXPECT_TRUE(leaf->all_equal);
    EXPECT_EQ(leaf->cardinality, 1u);
    ASSERT_EQ(leaf->size(), 5u);
    for (const auto &value : leaf->values) {
        EXPECT_EQ(BitUtil::get_i32_be(value, 0), 42);
    }
}

TEST(BkdLeafReaderTest, SkipsTheBoundsOfMultiDimensionalIndexes) {
    const BKDConfig config = config_of(2, 2);
    const std::vector<std::vector<uint8_t>> points{
        pack2(0x01000001, 0x10000005),
        pack2(0x02000002, 0x10000005),
        pack2(0x02000002, 0x20000009),
        pack2(0x03000003, 0x20000009),
    };
    const auto block = write_block(config, points);

    MemoryIndexInput in(block);
    auto leaf = BkdLeafReader::read_leaf_block(in, config, 0);
    ASSERT_TRUE(leaf.has_value()) << (leaf ? "" : leaf.error().to_string());

    ASSERT_EQ(leaf->values.size(), points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        EXPECT_EQ(leaf->values[i], points[i]) << "point " << i;
    }
    EXPECT_EQ(in.get_file_pointer().value(), block.size());
}

TEST(BkdLeafReaderTest, ReadsEveryBlockOfAWrittenTree) {
    const BKDConfig config = config_of(1, 1, 8);
    auto writer_result = BkdWriter::create(config, 40, 100);
    ASSERT_TRUE(writer_result.has_value());
    auto writer = std::move(writer_result.value());
    for (int32_t i = 0; i < 40; ++i) {
        ASSERT_TRUE(writer.add(pack1(i), i).has_value());
    }

    // BkdWriter wants an IndexOutput; a small adapter keeps the test self contained.
    class BufferIndexOutput final : public IndexOutput {
    public:
        using DataOutput::write_bytes;

        [[nodiscard]] VoidResult write_byte(uint8_t b) override {
            bytes.push_back(b);
            return {};
        }
        [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override {
            bytes.insert(bytes.end(), b.begin() + static_cast<std::ptrdiff_t>(offset),
                         b.begin() + static_cast<std::ptrdiff_t>(offset + length));
            return {};
        }
        [[nodiscard]] Result<size_t> get_file_pointer() const override { return bytes.size(); }
        [[nodiscard]] uint64_t get_checksum() override { return 0; }
        [[nodiscard]] std::string_view get_name() const override { return "buffer"; }
        [[nodiscard]] VoidResult close() override { return {}; }

        std::vector<uint8_t> bytes;
    } buffer;

    auto result = writer.finish(buffer);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    ASSERT_EQ(result->leaf_block_fps.size(), 5u);  // ceil(40 / 8)

    MemoryIndexInput in(buffer.bytes);
    std::vector<int32_t> values;
    std::vector<int32_t> doc_ids;
    for (const size_t block_fp : result->leaf_block_fps) {
        auto leaf = BkdLeafReader::read_leaf_block(in, config, block_fp);
        ASSERT_TRUE(leaf.has_value()) << (leaf ? "" : leaf.error().to_string());
        EXPECT_EQ(leaf->size(), 8u);
        for (size_t i = 0; i < leaf->size(); ++i) {
            values.push_back(BitUtil::get_i32_be(leaf->values[i], 0));
            doc_ids.push_back(leaf->doc_ids[i]);
        }
    }

    ASSERT_EQ(values.size(), 40u);
    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(values[i], static_cast<int32_t>(i));
        EXPECT_EQ(doc_ids[i], static_cast<int32_t>(i));
    }
}

TEST(BkdLeafReaderTest, RejectsCorruptBlocks) {
    const BKDConfig config = config_of(1, 1);

    {
        // count = 2, raw doc ids, no prefix, then a marker that is neither 0xFE
        // nor a valid compressed dimension (num_dims == 1 here).
        GrowableByteArrayDataOutput out;
        ASSERT_TRUE(out.write_vint(2).has_value());
        ASSERT_TRUE(out.write_byte(static_cast<uint8_t>(BkdDocIdsWriter::BPV_32)).has_value());
        ASSERT_TRUE(out.write_int(0).has_value());
        ASSERT_TRUE(out.write_int(1).has_value());
        ASSERT_TRUE(out.write_vint(0).has_value());
        ASSERT_TRUE(out.write_byte(0x05).has_value());
        const auto block = out.take_bytes();

        MemoryIndexInput in(block);
        auto leaf = BkdLeafReader::read_leaf_block(in, config, 0);
        ASSERT_FALSE(leaf.has_value());
        EXPECT_EQ(leaf.error().kind(), ErrorKind::kCorruptIndex);
    }

    {
        GrowableByteArrayDataOutput out;
        ASSERT_TRUE(out.write_vint(0).has_value());
        const auto block = out.take_bytes();

        MemoryIndexInput in(block);
        auto leaf = BkdLeafReader::read_leaf_block(in, config, 0);
        ASSERT_FALSE(leaf.has_value());
        EXPECT_EQ(leaf.error().kind(), ErrorKind::kCorruptIndex);
    }

    {
        // A run-length marker with no runs behind it: truncated input.
        GrowableByteArrayDataOutput out;
        ASSERT_TRUE(out.write_vint(2).has_value());
        ASSERT_TRUE(out.write_byte(static_cast<uint8_t>(BkdDocIdsWriter::BPV_32)).has_value());
        ASSERT_TRUE(out.write_int(0).has_value());
        ASSERT_TRUE(out.write_int(1).has_value());
        ASSERT_TRUE(out.write_vint(0).has_value());
        ASSERT_TRUE(out.write_byte(0x00).has_value());  // compressed dim 0
        const auto block = out.take_bytes();

        MemoryIndexInput in(block);
        EXPECT_FALSE(BkdLeafReader::read_leaf_block(in, config, 0).has_value());
    }
}
