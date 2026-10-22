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

module lucene_core:ut.bkd_index_tree;

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

/// Collects every packed value stored below `node`.
bool collect_points(const BkdIndexTree &tree, size_t node, IndexInput &data_in, const BKDConfig &config,
                    std::vector<std::vector<uint8_t>> &points) {
    const BkdIndexNode &current = tree.nodes[node];
    if (current.leaf) {
        auto leaf = BkdLeafReader::read_leaf_block(data_in, config, current.leaf_block_fp);
        if (!leaf.has_value()) {
            return false;
        }
        points.insert(points.end(), leaf->values.begin(), leaf->values.end());
        return true;
    }
    return collect_points(tree, current.left, data_in, config, points) &&
           collect_points(tree, current.right, data_in, config, points);
}

/// Leaf node indices in the order their blocks are written.
void collect_leaves(const BkdIndexTree &tree, size_t node, std::vector<size_t> &leaves) {
    const BkdIndexNode &current = tree.nodes[node];
    if (current.leaf) {
        leaves.push_back(node);
        return;
    }
    collect_leaves(tree, current.left, leaves);
    collect_leaves(tree, current.right, leaves);
}

/// Builds a written tree and returns (data bytes, meta bytes, writer result).
struct WrittenTree {
    std::vector<uint8_t> data;
    std::vector<uint8_t> meta;
    BkdWriteResult result;
};

WrittenTree write_tree(const BKDConfig &config, const std::vector<std::vector<uint8_t>> &points, int32_t max_doc) {
    WrittenTree written;
    auto writer_result = BkdWriter::create(config, points.size(), max_doc);
    EXPECT_TRUE(writer_result.has_value());
    auto writer = std::move(writer_result.value());
    for (size_t i = 0; i < points.size(); ++i) {
        EXPECT_TRUE(writer.add(points[i], static_cast<int32_t>(i)).has_value());
    }

    MemoryIndexOutput data;
    auto result = writer.finish(data);
    EXPECT_TRUE(result.has_value());
    written.result = std::move(result.value());
    written.data = data.bytes();

    MemoryIndexOutput meta;
    EXPECT_TRUE(writer.write_index(meta, nullptr, written.result).has_value());
    written.meta = meta.bytes();
    return written;
}

} // namespace

TEST(BkdIndexTreeTest, ReadsBackAWrittenTree) {
    const BKDConfig config = config_of(1, 1, 8);
    std::vector<std::vector<uint8_t>> points;
    for (int32_t i = 0; i < 40; ++i) {
        points.push_back(pack1(i));
    }
    const WrittenTree written = write_tree(config, points, 100);

    ByteArrayDataInput meta_in(written.meta);
    auto meta = BkdMeta::read(meta_in);
    ASSERT_TRUE(meta.has_value()) << (meta ? "" : meta.error().to_string());
    EXPECT_EQ(meta->config.num_dims, 1u);
    EXPECT_EQ(meta->config.num_index_dims, 1u);
    EXPECT_EQ(meta->config.bytes_per_dim, 4u);
    EXPECT_EQ(meta->config.max_points_in_leaf_node, 8u);
    EXPECT_EQ(meta->num_leaves, 5u);
    EXPECT_EQ(meta->point_count, 40);
    EXPECT_EQ(meta->doc_count, 40);
    EXPECT_EQ(static_cast<size_t>(meta->index_len), written.result.packed_index.size());
    EXPECT_EQ(BitUtil::get_i32_be(meta->min_value, 0), 0);
    EXPECT_EQ(BitUtil::get_i32_be(meta->max_value, 0), 39);

    MemoryIndexInput index_in(written.meta);
    auto tree = BkdIndexTree::read(index_in, *meta);
    ASSERT_TRUE(tree.has_value()) << (tree ? "" : tree.error().to_string());

    EXPECT_EQ(tree->num_leaves(), 5u);
    ASSERT_EQ(tree->nodes.size(), 9u);  // 2 * leaves - 1
    EXPECT_FALSE(tree->root().leaf);
    EXPECT_EQ(tree->root().min_value, std::vector<uint8_t>({0, 0, 0, 0}));
    EXPECT_EQ(tree->root().max_value, std::vector<uint8_t>({0, 0, 0, 39}));

    std::vector<size_t> leaves;
    collect_leaves(*tree, 0, leaves);
    ASSERT_EQ(leaves.size(), written.result.leaf_block_fps.size());
    for (size_t i = 0; i < leaves.size(); ++i) {
        EXPECT_EQ(tree->nodes[leaves[i]].leaf_block_fp, written.result.leaf_block_fps[i]) << "leaf " << i;
    }

    // Every internal node separates its two subtrees on the dimension it stores.
    MemoryIndexInput data_in(written.data);
    for (size_t node = 0; node < tree->nodes.size(); ++node) {
        const BkdIndexNode &current = tree->nodes[node];
        if (current.leaf) {
            continue;
        }
        ASSERT_EQ(current.split_value.size(), config.bytes_per_dim);

        std::vector<std::vector<uint8_t>> left_points;
        std::vector<std::vector<uint8_t>> right_points;
        ASSERT_TRUE(collect_points(*tree, current.left, data_in, config, left_points));
        ASSERT_TRUE(collect_points(*tree, current.right, data_in, config, right_points));
        ASSERT_FALSE(left_points.empty());
        ASSERT_FALSE(right_points.empty());

        for (const auto &point : left_points) {
            EXPECT_LE(BKDUtil::compare_unsigned(point, current.dim * config.bytes_per_dim, current.split_value, 0,
                                                config.bytes_per_dim),
                      0)
                << "node " << node << " left";
            EXPECT_GE(BKDUtil::compare_unsigned(point, 0, current.min_value, 0, config.packed_index_bytes_length()), 0);
            EXPECT_LE(BKDUtil::compare_unsigned(point, 0, current.max_value, 0, config.packed_index_bytes_length()), 0);
        }
        for (const auto &point : right_points) {
            EXPECT_GE(BKDUtil::compare_unsigned(point, current.dim * config.bytes_per_dim, current.split_value, 0,
                                                config.bytes_per_dim),
                      0)
                << "node " << node << " right";
        }
    }

    // Walking the tree in order returns every point exactly once, ascending.
    std::vector<std::vector<uint8_t>> all_points;
    ASSERT_TRUE(collect_points(*tree, 0, data_in, config, all_points));
    ASSERT_EQ(all_points.size(), 40u);
    for (size_t i = 0; i < all_points.size(); ++i) {
        EXPECT_EQ(BitUtil::get_i32_be(all_points[i], 0), static_cast<int32_t>(i));
    }
}

TEST(BkdIndexTreeTest, ReadsBackAMultiDimensionalTree) {
    const BKDConfig config = config_of(2, 2, 6);
    std::vector<std::vector<uint8_t>> points;
    for (int32_t i = 0; i < 20; ++i) {
        points.push_back(pack2(i, 100 - i));
    }
    const WrittenTree written = write_tree(config, points, 40);

    ByteArrayDataInput meta_in(written.meta);
    auto meta = BkdMeta::read(meta_in);
    ASSERT_TRUE(meta.has_value()) << (meta ? "" : meta.error().to_string());
    EXPECT_EQ(meta->config.num_dims, 2u);
    EXPECT_EQ(meta->config.num_index_dims, 2u);
    EXPECT_EQ(meta->num_leaves, 4u);
    EXPECT_EQ(meta->config.packed_index_bytes_length(), 8u);

    MemoryIndexInput index_in(written.meta);
    auto tree = BkdIndexTree::read(index_in, *meta);
    ASSERT_TRUE(tree.has_value()) << (tree ? "" : tree.error().to_string());
    ASSERT_EQ(tree->nodes.size(), 7u);

    // Bounds are stored for both indexed dimensions.
    EXPECT_EQ(tree->root().min_value, pack2(0, 81));
    EXPECT_EQ(tree->root().max_value, pack2(19, 100));

    MemoryIndexInput data_in(written.data);
    std::vector<std::vector<uint8_t>> all_points;
    ASSERT_TRUE(collect_points(*tree, 0, data_in, config, all_points));
    EXPECT_EQ(all_points.size(), 20u);

    for (size_t node = 0; node < tree->nodes.size(); ++node) {
        const BkdIndexNode &current = tree->nodes[node];
        if (current.leaf) {
            continue;
        }
        std::vector<std::vector<uint8_t>> left_points;
        std::vector<std::vector<uint8_t>> right_points;
        ASSERT_TRUE(collect_points(*tree, current.left, data_in, config, left_points));
        ASSERT_TRUE(collect_points(*tree, current.right, data_in, config, right_points));
        for (const auto &point : left_points) {
            EXPECT_LE(BKDUtil::compare_unsigned(point, current.dim * config.bytes_per_dim, current.split_value, 0,
                                                config.bytes_per_dim),
                      0)
                << "node " << node;
        }
        for (const auto &point : right_points) {
            EXPECT_GE(BKDUtil::compare_unsigned(point, current.dim * config.bytes_per_dim, current.split_value, 0,
                                                config.bytes_per_dim),
                      0)
                << "node " << node;
        }
    }
}

TEST(BkdIndexTreeTest, ReadsTheIndexFromASeparateFile) {
    const BKDConfig config = config_of(1, 1, 8);
    std::vector<std::vector<uint8_t>> points;
    for (int32_t i = 0; i < 20; ++i) {
        points.push_back(pack1(i * 5));
    }

    auto writer_result = BkdWriter::create(config, points.size(), 40);
    ASSERT_TRUE(writer_result.has_value());
    auto writer = std::move(writer_result.value());
    for (size_t i = 0; i < points.size(); ++i) {
        ASSERT_TRUE(writer.add(points[i], static_cast<int32_t>(i)).has_value());
    }
    MemoryIndexOutput data;
    auto result = writer.finish(data);
    ASSERT_TRUE(result.has_value());

    MemoryIndexOutput meta_out;
    MemoryIndexOutput index_out;
    ASSERT_TRUE(writer.write_index(meta_out, &index_out, *result).has_value());

    ByteArrayDataInput meta_in(meta_out.bytes());
    auto meta = BkdMeta::read(meta_in);
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->index_fp, 0);  // the index is its own file

    MemoryIndexInput index_in(index_out.bytes());
    auto tree = BkdIndexTree::read(index_in, *meta);
    ASSERT_TRUE(tree.has_value()) << (tree ? "" : tree.error().to_string());
    EXPECT_EQ(tree->num_leaves(), 3u);  // ceil(20 / 8)
}

TEST(BkdIndexTreeTest, RejectsInconsistentIndexLengths) {
    const BKDConfig config = config_of(1, 1, 8);
    std::vector<std::vector<uint8_t>> points;
    for (int32_t i = 0; i < 12; ++i) {
        points.push_back(pack1(i));
    }
    const WrittenTree written = write_tree(config, points, 20);

    ByteArrayDataInput meta_in(written.meta);
    auto meta = BkdMeta::read(meta_in);
    ASSERT_TRUE(meta.has_value());

    {
        // One byte too long: the decoded tree cannot match it.
        BkdMeta broken = *meta;
        broken.index_len += 1;
        MemoryIndexInput index_in(written.meta);
        auto tree = BkdIndexTree::read(index_in, broken);
        ASSERT_FALSE(tree.has_value());
        EXPECT_EQ(tree.error().kind(), ErrorKind::kCorruptIndex);
    }
    {
        // One byte too short.
        BkdMeta broken = *meta;
        broken.index_len -= 1;
        MemoryIndexInput index_in(written.meta);
        auto tree = BkdIndexTree::read(index_in, broken);
        ASSERT_FALSE(tree.has_value());
        EXPECT_EQ(tree.error().kind(), ErrorKind::kCorruptIndex);
    }
    {
        // The index claims to sit past the end of the input.
        BkdMeta broken = *meta;
        broken.index_fp = static_cast<int64_t>(written.meta.size());
        MemoryIndexInput index_in(written.meta);
        auto tree = BkdIndexTree::read(index_in, broken);
        ASSERT_FALSE(tree.has_value());
        EXPECT_EQ(tree.error().kind(), ErrorKind::kCorruptIndex);
    }
}

TEST(BkdIndexTreeTest, RejectsBadMetaHeaders) {
    {
        // Not a BKD header at all.
        const std::vector<uint8_t> garbage{1, 2, 3, 4, 5, 6, 7, 8};
        ByteArrayDataInput meta_in(garbage);
        EXPECT_FALSE(BkdMeta::read(meta_in).has_value());
    }
    {
        const BKDConfig config = config_of(1, 1, 8);
        std::vector<std::vector<uint8_t>> points{pack1(1), pack1(2)};
        const WrittenTree written = write_tree(config, points, 4);

        // Truncating the meta file mid-header must be reported, not crash.
        const std::vector<uint8_t> truncated(written.meta.begin(), written.meta.begin() + 10);
        ByteArrayDataInput meta_in(truncated);
        EXPECT_FALSE(BkdMeta::read(meta_in).has_value());
    }
}
