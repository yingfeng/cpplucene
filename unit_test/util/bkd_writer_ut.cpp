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

module lucene_core:ut.bkd_writer;

import lucene_core;

using namespace lucene;

namespace {

BKDConfig config_of(size_t dims, size_t index_dims, size_t max_points_in_leaf) {
    auto config = BKDConfig::create(dims, index_dims, 4, max_points_in_leaf);
    EXPECT_TRUE(config.has_value());
    return config.value();
}

std::vector<uint8_t> pack1(int32_t dim0) {
    std::vector<uint8_t> packed(4, 0);
    BitUtil::set_i32_be(packed, 0, dim0);
    return packed;
}

/// A [`IndexOutput`] that keeps everything in memory.
///
/// IndexOutput is abstract and the shipped implementations are private to the
/// directory code, so the tests bring their own.
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

class MemoryIndexOutput final : public IndexOutput {
public:
    using DataOutput::write_bytes;

    [[nodiscard]] VoidResult write_byte(uint8_t b) override {
        bytes_.push_back(b);
        return {};
    }
    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override {
        bytes_.insert(bytes_.end(), b.begin() + static_cast<std::ptrdiff_t>(offset),
                      b.begin() + static_cast<std::ptrdiff_t>(offset + length));
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

} // namespace

TEST(BkdWriterTest, WritesEveryLeafAndPacksTheIndex) {
    const BKDConfig config = config_of(1, 1, 8);
    auto writer_result = BkdWriter::create(config, 100, 200);
    ASSERT_TRUE(writer_result.has_value()) << (writer_result ? "" : writer_result.error().to_string());
    auto writer = std::move(writer_result.value());

    for (int32_t i = 0; i < 100; ++i) {
        ASSERT_TRUE(writer.add(pack1(i), i).has_value());
    }
    EXPECT_EQ(writer.point_count(), 100u);
    EXPECT_EQ(writer.doc_count(), 100);

    MemoryIndexOutput data;
    auto result = writer.finish(data);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

    EXPECT_EQ(result->point_count, 100u);
    EXPECT_EQ(result->doc_count, 100);
    EXPECT_EQ(result->count_per_leaf, 8u);
    EXPECT_EQ(result->data_start_fp, 0u);
    EXPECT_FALSE(result->packed_index.empty());

    // ceil(100 / 8) = 13 leaves: twelve of eight points plus a tail of four.
    ASSERT_EQ(result->leaf_block_fps.size(), 13u);
    EXPECT_EQ(result->leaf_block_fps[0], 0u);
    for (size_t i = 1; i < result->leaf_block_fps.size(); ++i) {
        EXPECT_LT(result->leaf_block_fps[i - 1], result->leaf_block_fps[i]);
    }

    MemoryIndexInput in(data.bytes());
    std::vector<int32_t> values;
    std::vector<int32_t> doc_ids;
    for (size_t i = 0; i < result->leaf_block_fps.size(); ++i) {
        auto leaf = BkdLeafReader::read_leaf_block(in, config, result->leaf_block_fps[i]);
        ASSERT_TRUE(leaf.has_value()) << "leaf " << i << ": " << (leaf ? "" : leaf.error().to_string());
        EXPECT_EQ(leaf->size(), i + 1 == result->leaf_block_fps.size() ? 4u : 8u) << "leaf " << i;
        for (size_t j = 0; j < leaf->size(); ++j) {
            values.push_back(BitUtil::get_i32_be(leaf->values[j], 0));
            doc_ids.push_back(leaf->doc_ids[j]);
        }
    }

    // The blocks are in leaf order and every leaf is sorted, so the flattened
    // values come back ascending; the doc ids travel with them.
    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(values[i], static_cast<int32_t>(i));
        EXPECT_EQ(doc_ids[i], static_cast<int32_t>(i));
    }

    // Blocks are written back to back, so the last one ends at the end of the file.
    MemoryIndexInput tail(data.bytes());
    auto last = BkdLeafReader::read_leaf_block(tail, config, result->leaf_block_fps.back());
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(tail.get_file_pointer().value(), data.bytes().size());
}

TEST(BkdWriterTest, WritesMetaAndIndexIntoOneFile) {
    const BKDConfig config = config_of(1, 1, 8);
    auto writer_result = BkdWriter::create(config, 40, 100);
    ASSERT_TRUE(writer_result.has_value());
    auto writer = std::move(writer_result.value());
    for (int32_t i = 0; i < 40; ++i) {
        ASSERT_TRUE(writer.add(pack1(i), i).has_value());
    }
    MemoryIndexOutput data;
    auto result = writer.finish(data);
    ASSERT_TRUE(result.has_value());

    MemoryIndexOutput meta;
    ASSERT_TRUE(writer.write_index(meta, nullptr, *result).has_value());

    ByteArrayDataInput in(meta.bytes());
    // The codec header is big endian (Lucene compatible), so read it through
    // CodecUtil rather than with the little endian `read_int`.
    auto version = CodecUtil::check_header(in, BKD_CODEC_NAME, BKD_VERSION_CURRENT, BKD_VERSION_CURRENT);
    ASSERT_TRUE(version.has_value()) << (version ? "" : version.error().to_string());
    EXPECT_EQ(version.value(), BKD_VERSION_CURRENT);

    EXPECT_EQ(in.read_vint().value(), 1);   // num_dims
    EXPECT_EQ(in.read_vint().value(), 1);   // num_index_dims
    EXPECT_EQ(in.read_vint().value(), 8);   // count_per_leaf
    EXPECT_EQ(in.read_vint().value(), 4);   // bytes_per_dim
    EXPECT_EQ(in.read_vint().value(), 5);   // ceil(40 / 8) leaves

    const auto min_value = in.get_bytes(4);
    ASSERT_TRUE(min_value.has_value());
    EXPECT_EQ(BitUtil::get_i32_be(*min_value, 0), 0);
    const auto max_value = in.get_bytes(4);
    ASSERT_TRUE(max_value.has_value());
    EXPECT_EQ(BitUtil::get_i32_be(*max_value, 0), 39);

    EXPECT_EQ(in.read_vlong().value(), 40);
    EXPECT_EQ(in.read_vint().value(), 40);  // distinct docs
    const int32_t index_len = in.read_vint().value();
    EXPECT_EQ(static_cast<size_t>(index_len), result->packed_index.size());
    EXPECT_EQ(in.read_long().value(), 0);  // data start fp

    const int64_t index_fp = in.read_long().value();
    // The index pointer is the position of the index inside this same file.
    EXPECT_EQ(static_cast<size_t>(index_fp), meta.bytes().size() - result->packed_index.size());
    const std::vector<uint8_t> index_bytes(meta.bytes().begin() + static_cast<std::ptrdiff_t>(index_fp),
                                           meta.bytes().end());
    EXPECT_EQ(index_bytes, result->packed_index);
}

TEST(BkdWriterTest, WritesIndexToItsOwnFile) {
    const BKDConfig config = config_of(1, 1, 8);
    auto writer_result = BkdWriter::create(config, 20, 50);
    ASSERT_TRUE(writer_result.has_value());
    auto writer = std::move(writer_result.value());
    for (int32_t i = 0; i < 20; ++i) {
        ASSERT_TRUE(writer.add(pack1(i * 3), i).has_value());
    }
    MemoryIndexOutput data;
    auto result = writer.finish(data);
    ASSERT_TRUE(result.has_value());

    MemoryIndexOutput meta_single;
    ASSERT_TRUE(writer.write_index(meta_single, nullptr, *result).has_value());

    MemoryIndexOutput meta;
    MemoryIndexOutput index;
    ASSERT_TRUE(writer.write_index(meta, &index, *result).has_value());

    EXPECT_EQ(index.bytes(), result->packed_index);
    // The index no longer lives in the meta file.
    EXPECT_EQ(meta.bytes().size() + result->packed_index.size(), meta_single.bytes().size());

    ByteArrayDataInput in(meta.bytes());
    ASSERT_TRUE(CodecUtil::check_header(in, BKD_CODEC_NAME, BKD_VERSION_CURRENT, BKD_VERSION_CURRENT).has_value());
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(in.read_vint().has_value());
    }
    ASSERT_TRUE(in.get_bytes(8).has_value());  // min + max
    ASSERT_TRUE(in.read_vlong().has_value());
    ASSERT_TRUE(in.read_vint().has_value());
    ASSERT_TRUE(in.read_vint().has_value());
    ASSERT_TRUE(in.read_long().has_value());
    // Pointing at the separate file, which is empty when the pointer is taken.
    EXPECT_EQ(in.read_long().value(), 0);
}

TEST(BkdWriterTest, RejectsBadAddsAndDuplicateFinish) {
    const BKDConfig config = config_of(1, 1, 8);
    {
        auto writer_result = BkdWriter::create(config, 4, 10);
        ASSERT_TRUE(writer_result.has_value());
        auto writer = std::move(writer_result.value());

        EXPECT_FALSE(writer.add(std::span<const uint8_t>(), 0).has_value());       // wrong size
        EXPECT_FALSE(writer.add(pack1(1), -1).has_value());                        // negative doc id
        EXPECT_FALSE(writer.add(pack1(1), 10).has_value());                        // past max_doc
        ASSERT_TRUE(writer.add(pack1(1), 0).has_value());

        MemoryIndexOutput data;
        EXPECT_TRUE(writer.finish(data).has_value());
        EXPECT_FALSE(writer.finish(data).has_value());  // already finished
        EXPECT_FALSE(writer.add(pack1(2), 1).has_value());
    }

    EXPECT_FALSE(BkdWriter::create(config, 0, 10).has_value());   // no room
    EXPECT_FALSE(BkdWriter::create(config, 10, 0).has_value());   // no documents

    auto small = BkdWriter::create(config, 2, 10);
    ASSERT_TRUE(small.has_value());
    auto writer = std::move(small.value());
    ASSERT_TRUE(writer.add(pack1(1), 0).has_value());
    ASSERT_TRUE(writer.add(pack1(2), 1).has_value());
    EXPECT_FALSE(writer.add(pack1(3), 2).has_value());  // more than announced
}

TEST(BkdWriterTest, TracksDocCountAndBounds) {
    const BKDConfig config = config_of(1, 1, 8);
    auto writer_result = BkdWriter::create(config, 10, 100);
    ASSERT_TRUE(writer_result.has_value());
    auto writer = std::move(writer_result.value());

    ASSERT_TRUE(writer.add(pack1(5), 0).has_value());
    ASSERT_TRUE(writer.add(pack1(3), 1).has_value());
    ASSERT_TRUE(writer.add(pack1(9), 1).has_value());  // same document again

    EXPECT_EQ(writer.point_count(), 3u);
    EXPECT_EQ(writer.doc_count(), 2);
    EXPECT_EQ(BitUtil::get_i32_be(writer.min_packed_value(), 0), 3);
    EXPECT_EQ(BitUtil::get_i32_be(writer.max_packed_value(), 0), 9);
}

TEST(BkdWriterTest, EmptyWriterProducesNoLeaves) {
    const BKDConfig config = config_of(1, 1, 8);
    auto writer_result = BkdWriter::create(config, 4, 10);
    ASSERT_TRUE(writer_result.has_value());
    auto writer = std::move(writer_result.value());

    MemoryIndexOutput data;
    auto result = writer.finish(data);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->point_count, 0u);
    EXPECT_TRUE(result->leaf_block_fps.empty());
    EXPECT_TRUE(result->packed_index.empty());
    EXPECT_TRUE(data.bytes().empty());

    MemoryIndexOutput meta;
    EXPECT_FALSE(writer.write_index(meta, nullptr, *result).has_value());
}

TEST(BkdWriterTest, PacksTheIndexForPermutedInput) {
    // Insertion order differs from value order, so the leaf boundaries fall on
    // values that are not at round positions.  The packed index stores the split
    // values as byte deltas and therefore needs them to be real boundaries, not
    // whatever low bytes the point at the boundary happens to carry.
    const BKDConfig config = config_of(1, 1, 8);
    auto writer_result = BkdWriter::create(config, 100, 200);
    ASSERT_TRUE(writer_result.has_value());
    auto writer = std::move(writer_result.value());
    for (int32_t i = 0; i < 100; ++i) {
        ASSERT_TRUE(writer.add(pack1((i * 37) % 100), i).has_value());
    }
    MemoryIndexOutput data;
    auto result = writer.finish(data);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    ASSERT_EQ(result->leaf_block_fps.size(), 13u);

    MemoryIndexInput in(data.bytes());
    int32_t previous = -1;
    size_t seen = 0;
    for (size_t i = 0; i < result->leaf_block_fps.size(); ++i) {
        auto leaf = BkdLeafReader::read_leaf_block(in, config, result->leaf_block_fps[i]);
        ASSERT_TRUE(leaf.has_value()) << "leaf " << i << ": " << (leaf ? "" : leaf.error().to_string());
        for (size_t j = 0; j < leaf->size(); ++j) {
            const int32_t value = BitUtil::get_i32_be(leaf->values[j], 0);
            EXPECT_GT(value, previous) << "leaf " << i << " index " << j;
            previous = value;
            ++seen;
        }
    }
    EXPECT_EQ(seen, 100u);
}

TEST(BkdWriterTest, SpillsToDiskAndBuildsTheSamePoints) {
    const BKDConfig config = config_of(1, 1, 8);
    ByteBuffersDirectory dir;

    // The heap budget is below the announced count, so every point goes to the
    // temporary file and `finish` has to build the tree externally.
    auto writer_result = BkdWriter::create(config, 100, 200, &dir, /*max_points_in_heap=*/16);
    ASSERT_TRUE(writer_result.has_value()) << (writer_result ? "" : writer_result.error().to_string());
    auto writer = std::move(writer_result.value());
    ASSERT_TRUE(writer.spilled());

    // A permutation of 0..99, so the partitioning has to do real work.
    std::vector<int32_t> expected;
    for (int32_t i = 0; i < 100; ++i) {
        const int32_t value = (i * 37) % 100;
        expected.push_back(value);
        ASSERT_TRUE(writer.add(pack1(value), i).has_value());
    }
    std::sort(expected.begin(), expected.end());

    MemoryIndexOutput data;
    auto result = writer.finish(data);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_EQ(result->point_count, 100u);
    EXPECT_EQ(result->doc_count, 100);
    EXPECT_EQ(result->leaf_block_fps.size(), 13u);  // ceil(100 / 8)
    EXPECT_FALSE(result->packed_index.empty());

    // Every point comes back exactly once, and the leaf order is the value
    // order: the partition boundaries are real byte boundaries, which is also
    // what the packer's strictly-increasing split deltas rely on.
    MemoryIndexInput in(data.bytes());
    std::vector<int32_t> values;
    std::vector<int32_t> doc_ids;
    for (size_t i = 0; i < result->leaf_block_fps.size(); ++i) {
        auto leaf = BkdLeafReader::read_leaf_block(in, config, result->leaf_block_fps[i]);
        ASSERT_TRUE(leaf.has_value()) << "leaf " << i << ": " << (leaf ? "" : leaf.error().to_string());
        EXPECT_LE(leaf->size(), 8u) << "leaf " << i;
        for (size_t j = 0; j < leaf->size(); ++j) {
            values.push_back(BitUtil::get_i32_be(leaf->values[j], 0));
            doc_ids.push_back(leaf->doc_ids[j]);
        }
    }
    EXPECT_EQ(values, expected);
    std::vector<int32_t> sorted_doc_ids = doc_ids;
    std::sort(sorted_doc_ids.begin(), sorted_doc_ids.end());
    for (int32_t i = 0; i < 100; ++i) {
        EXPECT_EQ(sorted_doc_ids[static_cast<size_t>(i)], i);
    }
    // value -> doc id mapping survived the spill.
    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(static_cast<int32_t>(doc_ids[i] * 37 % 100), values[i]);
    }

    // The index is written from the external build's splits.
    MemoryIndexOutput meta;
    ASSERT_TRUE(writer.write_index(meta, nullptr, *result).has_value());

    // Only the spilled point file is left; the partition scratch files are gone.
    EXPECT_EQ(dir.file_count(), 1u);
    ASSERT_TRUE(writer.close().has_value());
    EXPECT_EQ(dir.file_count(), 0u);
}

TEST(BkdWriterTest, SpilledBuildHandlesASingleLeaf) {
    const BKDConfig config = config_of(1, 1, 8);
    ByteBuffersDirectory dir;
    auto writer_result = BkdWriter::create(config, 5, 10, &dir, /*max_points_in_heap=*/2);
    ASSERT_TRUE(writer_result.has_value());
    auto writer = std::move(writer_result.value());
    ASSERT_TRUE(writer.spilled());

    for (int32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(writer.add(pack1(4 - i), i).has_value());
    }
    MemoryIndexOutput data;
    auto result = writer.finish(data);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    ASSERT_EQ(result->leaf_block_fps.size(), 1u);
    EXPECT_FALSE(result->packed_index.empty());

    MemoryIndexInput in(data.bytes());
    auto leaf = BkdLeafReader::read_leaf_block(in, config, result->leaf_block_fps[0]);
    ASSERT_TRUE(leaf.has_value());
    ASSERT_EQ(leaf->size(), 5u);
    // A single-leaf tree has no parent split dimension, so the block keeps the
    // insertion order (the heap build behaves the same way).
    for (size_t i = 0; i < leaf->size(); ++i) {
        EXPECT_EQ(BitUtil::get_i32_be(leaf->values[i], 0), 4 - static_cast<int32_t>(i));
        EXPECT_EQ(leaf->doc_ids[i], static_cast<int32_t>(i));
    }
    ASSERT_TRUE(writer.close().has_value());
    EXPECT_EQ(dir.file_count(), 0u);
}

TEST(BkdWriterTest, WritesMultiDimensionalTrees) {
    const BKDConfig config = config_of(2, 2, 6);
    auto writer_result = BkdWriter::create(config, 20, 40);
    ASSERT_TRUE(writer_result.has_value());
    auto writer = std::move(writer_result.value());

    for (int32_t i = 0; i < 20; ++i) {
        std::vector<uint8_t> packed(8, 0);
        BitUtil::set_i32_be(packed, 0, i);
        BitUtil::set_i32_be(packed, 4, 100 - i);
        ASSERT_TRUE(writer.add(packed, i).has_value());
    }
    MemoryIndexOutput data;
    auto result = writer.finish(data);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_EQ(result->leaf_block_fps.size(), 4u);  // ceil(20 / 6)
    EXPECT_FALSE(result->packed_index.empty());
    EXPECT_EQ(BitUtil::get_i32_be(writer.min_packed_value(), 0), 0);
    EXPECT_EQ(BitUtil::get_i32_be(writer.min_packed_value(), 4), 81);
    EXPECT_EQ(BitUtil::get_i32_be(writer.max_packed_value(), 0), 19);
    EXPECT_EQ(BitUtil::get_i32_be(writer.max_packed_value(), 4), 100);
}
