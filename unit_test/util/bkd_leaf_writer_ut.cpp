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

module lucene_core:ut.bkd_leaf_writer;

import lucene_core;

using namespace lucene;

namespace {

constexpr uint8_t kAllEqual = 0xFF;

BKDConfig config_of(size_t dims, size_t index_dims) {
    auto config = BKDConfig::create(dims, index_dims, 4, 512);
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

} // namespace

TEST(BkdLeafWriterTest, WritesAllEqualBlockAsOneByte) {
    const BKDConfig config = config_of(1, 1);
    HeapPointWriter writer(config, 5);
    for (int32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(writer.append_bytes(pack1(42), i).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    GrowableByteArrayDataOutput out;
    auto info = BkdLeafWriter::write_leaf_block(writer, BkdLeaf{0, 5}, out);
    ASSERT_TRUE(info.has_value()) << (info ? "" : info.error().to_string());

    EXPECT_EQ(info->count, 5u);
    EXPECT_EQ(info->cardinality, 1u);
    ASSERT_EQ(info->common_prefix_lengths.size(), 1u);
    EXPECT_EQ(info->common_prefix_lengths[0], 4u);
    EXPECT_EQ(out.bytes().back(), kAllEqual);

    MemoryIndexInput in(out.bytes());
    auto leaf = BkdLeafReader::read_leaf_block(in, config, 0);
    ASSERT_TRUE(leaf.has_value());
    EXPECT_TRUE(leaf->all_equal);
    EXPECT_EQ(leaf->doc_ids, (std::vector<int32_t>{0, 1, 2, 3, 4}));
    ASSERT_EQ(leaf->values.size(), 5u);
    for (const auto &value : leaf->values) {
        EXPECT_EQ(value, pack1(42));
    }
}

TEST(BkdLeafWriterTest, RunLengthEncodesRepeatedValues) {
    const BKDConfig config = config_of(1, 1);
    const std::vector<int32_t> dim0{0, 1, 2, 7, 7, 9};
    HeapPointWriter writer(config, dim0.size());
    for (size_t i = 0; i < dim0.size(); ++i) {
        ASSERT_TRUE(writer.append_bytes(pack1(dim0[i]), static_cast<int32_t>(i)).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    GrowableByteArrayDataOutput out;
    auto info = BkdLeafWriter::write_leaf_block(writer, BkdLeaf{0, dim0.size()}, out);
    ASSERT_TRUE(info.has_value()) << (info ? "" : info.error().to_string());

    // 0, 1, 2, 7 (twice), 9: five runs, and the low-cardinality form is the
    // cheaper of the two layouts here.
    EXPECT_EQ(info->cardinality, 5u);
    // Big-endian small integers share their leading zero bytes: `0` = `00 00 00
    // 00` and `9` = `00 00 00 09`, so three of the four bytes are common.
    EXPECT_EQ(info->common_prefix_lengths[0], 3u);

    MemoryIndexInput in(out.bytes());
    auto leaf = BkdLeafReader::read_leaf_block(in, config, 0);
    ASSERT_TRUE(leaf.has_value());
    EXPECT_FALSE(leaf->all_equal);
    EXPECT_FALSE(leaf->compressed_dim.has_value());
    EXPECT_EQ(leaf->cardinality, 5u);
    ASSERT_EQ(leaf->values.size(), dim0.size());
    for (size_t i = 0; i < dim0.size(); ++i) {
        EXPECT_EQ(leaf->values[i], pack1(dim0[i])) << "point " << i;
        EXPECT_EQ(leaf->doc_ids[i], static_cast<int32_t>(i));
    }
}

TEST(BkdLeafWriterTest, UsesTheRunLengthFormWhenEveryValueDiffers) {
    const BKDConfig config = config_of(1, 1);
    HeapPointWriter writer(config, 6);
    for (int32_t i = 0; i < 6; ++i) {
        ASSERT_TRUE(writer.append_bytes(pack1(i * 4), i).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    GrowableByteArrayDataOutput out;
    auto info = BkdLeafWriter::write_leaf_block(writer, BkdLeaf{0, 6, 0}, out);
    ASSERT_TRUE(info.has_value()) << (info ? "" : info.error().to_string());
    EXPECT_EQ(info->cardinality, 6u);
    EXPECT_EQ(info->sorted_dim, 0u);

    MemoryIndexInput in(out.bytes());
    auto leaf = BkdLeafReader::read_leaf_block(in, config, 0);
    ASSERT_TRUE(leaf.has_value()) << (leaf ? "" : leaf.error().to_string());
    EXPECT_FALSE(leaf->all_equal);
    ASSERT_TRUE(leaf->compressed_dim.has_value());
    EXPECT_EQ(leaf->compressed_dim.value(), 0u);
    ASSERT_EQ(leaf->values.size(), 6u);
    for (size_t i = 0; i < leaf->values.size(); ++i) {
        EXPECT_EQ(leaf->values[i], pack1(static_cast<int32_t>(i) * 4));
    }
}

TEST(BkdLeafWriterTest, StripsTheCommonPrefixOnNonIndexedDimension) {
    const BKDConfig config = config_of(2, 1);
    const std::vector<std::vector<uint8_t>> points{pack2(7, 1), pack2(7, 2), pack2(7, 2), pack2(7, 5)};
    HeapPointWriter writer(config, points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        ASSERT_TRUE(writer.append_bytes(points[i], static_cast<int32_t>(i)).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    GrowableByteArrayDataOutput out;
    auto info = BkdLeafWriter::write_leaf_block(writer, BkdLeaf{0, points.size()}, out);
    ASSERT_TRUE(info.has_value()) << (info ? "" : info.error().to_string());

    ASSERT_EQ(info->common_prefix_lengths.size(), 2u);
    EXPECT_EQ(info->common_prefix_lengths[0], 4u);  // indexed dim, constant
    EXPECT_EQ(info->common_prefix_lengths[1], 3u);  // data-only dim, big-endian 1..5
    EXPECT_EQ(info->cardinality, 3u);               // 1, 2 (twice), 5

    MemoryIndexInput in(out.bytes());
    auto leaf = BkdLeafReader::read_leaf_block(in, config, 0);
    ASSERT_TRUE(leaf.has_value()) << (leaf ? "" : leaf.error().to_string());
    ASSERT_EQ(leaf->values.size(), points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        EXPECT_EQ(leaf->values[i], points[i]) << "point " << i;
    }
}

TEST(BkdLeafWriterTest, ComputesTheCommonPrefixOverTheWholeBlock) {
    const BKDConfig config = config_of(2, 2);
    // The block is sorted on dimension 0 only; dimension 1 starts and ends with
    // the same value while the middle point differs, so a prefix derived from the
    // first and last value would truncate `0x01ff0304`.
    const std::vector<std::vector<uint8_t>> points{
        pack2(1, 0x01020304),
        pack2(2, 0x01FF0304),
        pack2(3, 0x01020304),
    };
    HeapPointWriter writer(config, points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        ASSERT_TRUE(writer.append_bytes(points[i], static_cast<int32_t>(i)).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    GrowableByteArrayDataOutput out;
    auto info = BkdLeafWriter::write_leaf_block(writer, BkdLeaf{0, points.size()}, out);
    ASSERT_TRUE(info.has_value()) << (info ? "" : info.error().to_string());

    EXPECT_EQ(info->common_prefix_lengths[0], 3u);  // 1, 2, 3 share three bytes
    EXPECT_EQ(info->common_prefix_lengths[1], 1u);  // 01 vs ff differ in the second byte

    MemoryIndexInput in(out.bytes());
    auto leaf = BkdLeafReader::read_leaf_block(in, config, 0);
    ASSERT_TRUE(leaf.has_value()) << (leaf ? "" : leaf.error().to_string());
    ASSERT_EQ(leaf->values.size(), points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        EXPECT_EQ(leaf->values[i], points[i]) << "point " << i;
    }
}

TEST(BkdLeafWriterTest, WritesBoundsForMultiDimensionalIndex) {
    const BKDConfig config = config_of(2, 2);
    // Leading bytes differ, so no dimension has a common prefix and the stored
    // suffixes are a full four bytes wide.
    const std::vector<std::vector<uint8_t>> points{
        pack2(0x01000001, 0x10000005),
        pack2(0x02000002, 0x10000005),
        pack2(0x02000002, 0x20000009),
        pack2(0x03000003, 0x20000009),
    };
    HeapPointWriter writer(config, points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        ASSERT_TRUE(writer.append_bytes(points[i], static_cast<int32_t>(i)).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    GrowableByteArrayDataOutput out;
    auto info = BkdLeafWriter::write_leaf_block(writer, BkdLeaf{0, points.size()}, out);
    ASSERT_TRUE(info.has_value()) << (info ? "" : info.error().to_string());

    EXPECT_EQ(info->common_prefix_lengths[0], 0u);
    EXPECT_EQ(info->common_prefix_lengths[1], 0u);
    EXPECT_EQ(info->cardinality, 4u);  // every value differs
    EXPECT_EQ(info->sorted_dim, 0u);

    MemoryIndexInput in(out.bytes());
    auto leaf = BkdLeafReader::read_leaf_block(in, config, 0);
    ASSERT_TRUE(leaf.has_value()) << (leaf ? "" : leaf.error().to_string());
    ASSERT_EQ(leaf->values.size(), points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        EXPECT_EQ(leaf->values[i], points[i]) << "point " << i;
    }

    // Bounds sit between the marker and the runs: min/max per indexed dim.  The
    // run-length form stores two bytes per run plus both suffixes per point.
    // One run per distinct byte of the compressed dimension: the compressed byte
    // itself is stored once per run, so a point's suffix is one byte shorter.
    const size_t per_point = (4 - 1) + 4;
    const size_t runs_bytes = (2 + 1 * per_point) + (2 + 2 * per_point) + (2 + 1 * per_point);
    const size_t bounds_bytes = 2 * 2 * 4;
    ASSERT_TRUE(out.bytes().size() > runs_bytes + bounds_bytes);
    const size_t marker_offset = out.bytes().size() - runs_bytes - bounds_bytes - 1;
    EXPECT_EQ(out.bytes()[marker_offset], static_cast<uint8_t>(info->sorted_dim));

    std::vector<uint8_t> expected_bounds;
    const auto append_be = [&expected_bounds](int32_t value) {
        const auto packed = pack1(value);
        expected_bounds.insert(expected_bounds.end(), packed.begin(), packed.end());
    };
    append_be(0x01000001);  // dim 0 min
    append_be(0x03000003);  // dim 0 max
    append_be(0x10000005);  // dim 1 min
    append_be(0x20000009);  // dim 1 max

    const auto bounds_begin = out.bytes().begin() + static_cast<std::ptrdiff_t>(marker_offset) + 1;
    const std::vector<uint8_t> bounds(bounds_begin, bounds_begin + static_cast<std::ptrdiff_t>(bounds_bytes));
    EXPECT_EQ(bounds, expected_bounds);
}

TEST(BkdLeafWriterTest, RejectsOpenWritersAndEmptyRanges) {
    const BKDConfig config = config_of(1, 1);
    HeapPointWriter writer(config, 4);
    ASSERT_TRUE(writer.append_bytes(pack1(1), 0).has_value());

    {
        GrowableByteArrayDataOutput out;
        // Still open.
        EXPECT_FALSE(BkdLeafWriter::write_leaf_block(writer, BkdLeaf{0, 1}, out).has_value());
    }

    ASSERT_TRUE(writer.close().has_value());
    {
        GrowableByteArrayDataOutput out;
        EXPECT_FALSE(BkdLeafWriter::write_leaf_block(writer, BkdLeaf{0, 0}, out).has_value());  // empty
        EXPECT_FALSE(BkdLeafWriter::write_leaf_block(writer, BkdLeaf{0, 2}, out).has_value());  // past the end
    }
}

TEST(BkdLeafWriterTest, WritesOneBlockPerLeaf) {
    const BKDConfig config = config_of(1, 1);
    HeapPointWriter writer(config, 16);
    for (int32_t i = 0; i < 16; ++i) {
        ASSERT_TRUE(writer.append_bytes(pack1(i * 3), i).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    BkdTreePlanner planner(writer, 4);
    auto leaves = planner.build();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_EQ(leaves->size(), 4u);

    GrowableByteArrayDataOutput out;
    std::vector<size_t> block_fps;
    for (const auto &leaf : *leaves) {
        block_fps.push_back(out.length());
        auto info = BkdLeafWriter::write_leaf_block(writer, leaf, out);
        ASSERT_TRUE(info.has_value()) << (info ? "" : info.error().to_string());
        EXPECT_EQ(info->count, leaf.count());
    }

    // Every leaf is a self-contained block and the reader finds them by pointer.
    MemoryIndexInput in(out.bytes());
    std::vector<int32_t> values;
    for (size_t i = 0; i < block_fps.size(); ++i) {
        auto leaf = BkdLeafReader::read_leaf_block(in, config, block_fps[i]);
        ASSERT_TRUE(leaf.has_value()) << (leaf ? "" : leaf.error().to_string());
        EXPECT_EQ(leaf->size(), 4u);
        for (const auto &value : leaf->values) {
            values.push_back(BitUtil::get_i32_be(value, 0));
        }
    }
    ASSERT_EQ(values.size(), 16u);
    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(values[i], static_cast<int32_t>(i) * 3);
    }
}
