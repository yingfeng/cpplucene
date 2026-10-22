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

module lucene_core:bkd_index_tree.impl;

import :bkd_index_tree;

import :bkd_config;
import :bkd_tree_planner;
import :codec_util;
import :data_input;
import :error;
import :index_input;
import :types;

import std;

namespace lucene {

namespace {

/// Reads a non-negative vint and widens it to `size_t`.
[[nodiscard]] Result<size_t> read_size(DataInput &in, std::string_view what) {
    auto value = in.read_vint();
    if (!value.has_value()) {
        return err(value.error());
    }
    if (value.value() < 0) {
        return err(LuceneError::corrupt_index(std::string(what) + " is negative"));
    }
    return static_cast<size_t>(value.value());
}

[[nodiscard]] LuceneError corrupt(std::string message) {
    return LuceneError::corrupt_index(std::move(message));
}

/// Reads one node of the packed index, then its children.
///
/// Mirrors rlucene's `read_node_data` + `push_left`/`push_right`, with the level
/// stacks replaced by copies handed down the recursion: `split_values` is the
/// per-dimension split value last seen on the path here, `negative_deltas`
/// records that a dimension descended on the way down (which flips the sign of
/// the changed-byte delta), and `min_value`/`max_value` are the bounds inherited
/// from the ancestors.
///
/// `is_left` says whether this subtree is its parent's left child, which decides
/// whether a leaf block pointer delta is stored.  `parent_index` is recorded on
/// the node so a [`PointTree`] cursor can walk back up.
[[nodiscard]] Result<size_t> parse_node(IndexInput &in, const BkdMeta &meta, BkdIndexTree &tree, bool is_left,
                                        size_t parent_index, size_t num_leaves, size_t min_block_fp,
                                        const std::vector<uint8_t> &split_values, std::vector<uint8_t> min_value,
                                        std::vector<uint8_t> max_value, std::vector<uint8_t> negative_deltas,
                                        std::optional<size_t> parent_dim) {
    const BKDConfig &config = meta.config;
    const size_t bytes_per_dim = config.bytes_per_dim;

    size_t block_fp = min_block_fp;
    if (!is_left) {
        auto delta = in.read_vlong();
        if (!delta.has_value()) {
            return err(delta.error());
        }
        if (delta.value() < 0) {
            return err(corrupt("the leaf block pointer delta is negative"));
        }
        block_fp += static_cast<size_t>(delta.value());
    }

    BkdIndexNode node;
    node.num_leaves = num_leaves;
    node.leaf_block_fp = block_fp;
    node.min_value = std::move(min_value);
    node.max_value = std::move(max_value);
    node.parent = parent_index;
    node.is_left = parent_index == kNoBkdParent ? false : is_left;

    const size_t node_index = tree.nodes.size();
    tree.nodes.push_back(std::move(node));
    if (num_leaves == 1) {
        tree.nodes[node_index].leaf = true;
        return node_index;
    }

    if (parent_dim.has_value()) {
        negative_deltas[parent_dim.value()] = is_left ? 1 : 0;
    }

    auto code = in.read_vint();
    if (!code.has_value()) {
        return err(code.error());
    }
    if (code.value() < 0) {
        return err(corrupt("the packed index node code is negative"));
    }
    int64_t remaining = code.value();
    const size_t split_dim = static_cast<size_t>(remaining % static_cast<int64_t>(config.num_index_dims));
    remaining /= static_cast<int64_t>(config.num_index_dims);
    const size_t prefix = static_cast<size_t>(remaining % static_cast<int64_t>(1 + bytes_per_dim));
    remaining /= static_cast<int64_t>(1 + bytes_per_dim);
    if (prefix > bytes_per_dim) {
        return err(corrupt("the split value prefix is out of range"));
    }
    const size_t suffix = bytes_per_dim - prefix;
    const size_t dim_base = split_dim * bytes_per_dim;

    // The changed byte rides in the code; the rest of the suffix follows raw.
    std::vector<uint8_t> current = split_values;
    if (suffix > 0) {
        int64_t delta = remaining;
        if (negative_deltas[split_dim] != 0) {
            delta = -delta;
        }
        const int32_t updated = static_cast<int32_t>(current[dim_base + prefix]) + static_cast<int32_t>(delta);
        if (updated < 0 || updated > 255) {
            return err(corrupt("the decoded split value byte is out of range"));
        }
        current[dim_base + prefix] = static_cast<uint8_t>(updated);
        if (suffix > 1) {
            if (auto read = in.read_bytes(
                    std::span<uint8_t>(current.data() + dim_base + prefix + 1, suffix - 1));
                !read.has_value()) {
                return err(read.error());
            }
        }
    }

    const size_t num_left = BkdTreePlanner::num_left_leaf_nodes(num_leaves);

    size_t left_num_bytes = 0;
    if (num_left > 1) {
        auto stored = in.read_vint();
        if (!stored.has_value()) {
            return err(stored.error());
        }
        if (stored.value() < 0) {
            return err(corrupt("the left subtree index length is negative"));
        }
        left_num_bytes = static_cast<size_t>(stored.value());
    }

    auto left_max = tree.nodes[node_index].max_value;
    auto right_min = tree.nodes[node_index].min_value;
    std::copy_n(current.begin() + static_cast<std::ptrdiff_t>(dim_base), bytes_per_dim,
                left_max.begin() + static_cast<std::ptrdiff_t>(dim_base));
    std::copy_n(current.begin() + static_cast<std::ptrdiff_t>(dim_base), bytes_per_dim,
                right_min.begin() + static_cast<std::ptrdiff_t>(dim_base));

    auto left_start = in.get_file_pointer();
    if (!left_start.has_value()) {
        return err(left_start.error());
    }

    auto left = parse_node(in, meta, tree, /*is_left=*/true, node_index, num_left, block_fp, current,
                           tree.nodes[node_index].min_value, std::move(left_max), negative_deltas, split_dim);
    if (!left.has_value()) {
        return err(left.error());
    }

    if (num_left > 1) {
        auto left_end = in.get_file_pointer();
        if (!left_end.has_value()) {
            return err(left_end.error());
        }
        if (left_end.value() - left_start.value() != left_num_bytes) {
            return err(corrupt("the stored left subtree length does not match the index"));
        }
    }

    auto right = parse_node(in, meta, tree, /*is_left=*/false, node_index, num_leaves - num_left, block_fp, current,
                            std::move(right_min), tree.nodes[node_index].max_value, negative_deltas, split_dim);
    if (!right.has_value()) {
        return err(right.error());
    }

    tree.nodes[node_index].dim = split_dim;
    tree.nodes[node_index].split_value.assign(current.begin() + static_cast<std::ptrdiff_t>(dim_base),
                                              current.begin() + static_cast<std::ptrdiff_t>(dim_base + bytes_per_dim));
    tree.nodes[node_index].left = left.value();
    tree.nodes[node_index].right = right.value();
    return node_index;
}

} // namespace

Result<BkdMeta> BkdMeta::read(DataInput &meta_in) {
    auto version = CodecUtil::check_header(meta_in, BKD_CODEC_NAME, BKD_VERSION_CURRENT, BKD_VERSION_CURRENT);
    if (!version.has_value()) {
        return err(version.error());
    }

    auto num_dims = read_size(meta_in, "num_dims");
    if (!num_dims.has_value()) {
        return err(num_dims.error());
    }
    if (num_dims.value() == 0 || num_dims.value() > BKDConfig::MAX_DIMS) {
        return err(corrupt("num_dims is out of range"));
    }
    auto num_index_dims = read_size(meta_in, "num_index_dims");
    if (!num_index_dims.has_value()) {
        return err(num_index_dims.error());
    }
    if (num_index_dims.value() == 0 || num_index_dims.value() > num_dims.value() ||
        num_index_dims.value() > BKDConfig::MAX_INDEX_DIMS) {
        return err(corrupt("num_index_dims is out of range"));
    }
    auto count_per_leaf = read_size(meta_in, "count_per_leaf");
    if (!count_per_leaf.has_value()) {
        return err(count_per_leaf.error());
    }
    auto bytes_per_dim = read_size(meta_in, "bytes_per_dim");
    if (!bytes_per_dim.has_value()) {
        return err(bytes_per_dim.error());
    }

    auto config = BKDConfig::create(num_dims.value(), num_index_dims.value(), bytes_per_dim.value(),
                                    count_per_leaf.value());
    if (!config.has_value()) {
        return err(corrupt("the stored BKD parameters are invalid: " + config.error().to_string()));
    }

    BkdMeta meta;
    meta.config = config.value();

    auto num_leaves = read_size(meta_in, "num_leaves");
    if (!num_leaves.has_value()) {
        return err(num_leaves.error());
    }
    if (num_leaves.value() == 0) {
        return err(corrupt("a BKD index must have at least one leaf"));
    }
    meta.num_leaves = num_leaves.value();

    const size_t index_bytes = meta.config.packed_index_bytes_length();
    auto min_value = meta_in.get_bytes(index_bytes);
    if (!min_value.has_value()) {
        return err(min_value.error());
    }
    meta.min_value = std::move(min_value.value());
    auto max_value = meta_in.get_bytes(index_bytes);
    if (!max_value.has_value()) {
        return err(max_value.error());
    }
    meta.max_value = std::move(max_value.value());

    auto point_count = meta_in.read_vlong();
    if (!point_count.has_value()) {
        return err(point_count.error());
    }
    if (point_count.value() < 0) {
        return err(corrupt("the point count is negative"));
    }
    meta.point_count = point_count.value();

    auto doc_count = meta_in.read_vint();
    if (!doc_count.has_value()) {
        return err(doc_count.error());
    }
    if (doc_count.value() < 0) {
        return err(corrupt("the document count is negative"));
    }
    meta.doc_count = doc_count.value();

    auto index_len = read_size(meta_in, "index_len");
    if (!index_len.has_value()) {
        return err(index_len.error());
    }
    meta.index_len = index_len.value();

    auto data_start_fp = meta_in.read_long();
    if (!data_start_fp.has_value()) {
        return err(data_start_fp.error());
    }
    meta.data_start_fp = data_start_fp.value();

    auto index_fp = meta_in.read_long();
    if (!index_fp.has_value()) {
        return err(index_fp.error());
    }
    if (index_fp.value() < 0) {
        return err(corrupt("the index file pointer is negative"));
    }
    meta.index_fp = index_fp.value();

    return meta;
}

Result<BkdIndexTree> BkdIndexTree::read(IndexInput &index_in, const BkdMeta &meta) {
    auto length = index_in.length();
    if (!length.has_value()) {
        return err(length.error());
    }
    const size_t index_start = static_cast<size_t>(meta.index_fp);
    if (index_start + meta.index_len > length.value()) {
        return err(corrupt("the packed index does not fit into the input"));
    }
    if (auto seeked = index_in.seek(index_start); !seeked.has_value()) {
        return err(seeked.error());
    }

    BkdIndexTree tree;
    // A full binary tree with n leaves has 2n-1 nodes; the cap keeps a corrupt
    // header from reserving absurd amounts of memory before it is validated.
    if (meta.num_leaves < (size_t{1} << 20)) {
        tree.nodes.reserve(2 * meta.num_leaves - 1);
    }

    std::vector<uint8_t> split_values(meta.config.packed_index_bytes_length(), 0);
    std::vector<uint8_t> negative_deltas(meta.config.num_index_dims, 0);

    auto root = parse_node(index_in, meta, tree, /*is_left=*/false, kNoBkdParent, meta.num_leaves, /*min_block_fp=*/0,
                           split_values, meta.min_value, meta.max_value, negative_deltas, std::nullopt);
    if (!root.has_value()) {
        return err(root.error());
    }

    auto end = index_in.get_file_pointer();
    if (!end.has_value()) {
        return err(end.error());
    }
    if (end.value() - index_start != meta.index_len) {
        return err(corrupt("the packed index length does not match the decoded tree"));
    }
    return tree;
}

} // namespace lucene
