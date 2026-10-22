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

module lucene_core:bkd_tree_reader.impl;

import :bkd_tree_reader;

import :bkd_config;
import :bkd_index_tree;
import :bkd_leaf_reader;
import :bkd_util;
import :error;
import :index_input;
import :types;

import std;

namespace lucene {

namespace {

/// Packs the per-dimension comparison result for one point.
///
/// Returns `true` when every indexed dimension of `value` lies in `[min, max)`.
[[nodiscard]] bool inside_box(std::span<const uint8_t> value, std::span<const uint8_t> min,
                              std::span<const uint8_t> max, const BKDConfig &config) {
    for (size_t dim = 0; dim < config.num_index_dims; ++dim) {
        const size_t offset = dim * config.bytes_per_dim;
        // A cell is a half-open box on the indexed dimensions.
        if (BKDUtil::compare_unsigned(value, offset, min, offset, config.bytes_per_dim) < 0) {
            return false;
        }
        if (BKDUtil::compare_unsigned(value, offset, max, offset, config.bytes_per_dim) >= 0) {
            return false;
        }
    }
    return true;
}

/// Visits `node_index` and its subtree, pruning cells that cannot intersect.
///
/// `min`/`max` are empty for [`BkdTreeReader::visit_all`], which turns the
/// comparison off instead of preparing an infinite box (an all-`0x00` min is a
/// valid bound, so a sentinel would be wrong).
[[nodiscard]] VoidResult visit_node(const BkdIndexTree &tree, size_t node_index, const BkdMeta &meta,
                                    IndexInput &data_in, std::span<const uint8_t> min,
                                    std::span<const uint8_t> max, const BkdPointVisitor &visitor) {
    const BkdIndexNode &node = tree.nodes[node_index];
    if (node.leaf) {
        auto leaf = BkdLeafReader::read_leaf_block(data_in, meta.config, node.leaf_block_fp);
        if (!leaf.has_value()) {
            return err(leaf.error());
        }
        for (size_t i = 0; i < leaf->size(); ++i) {
            if (!min.empty() && !inside_box(leaf->values[i], min, max, meta.config)) {
                continue;
            }
            if (auto visited = visitor(leaf->doc_ids[i], leaf->values[i]); !visited.has_value()) {
                return visited;
            }
        }
        return {};
    }

    bool visit_left = true;
    bool visit_right = true;
    if (!min.empty()) {
        // Every point in the left subtree is <= the split value on `node.dim`,
        // every point in the right subtree is >= it.
        const size_t offset = node.dim * meta.config.bytes_per_dim;
        if (BKDUtil::compare_unsigned(node.split_value, 0, max, offset, meta.config.bytes_per_dim) >= 0) {
            visit_right = false;
        }
        if (BKDUtil::compare_unsigned(node.split_value, 0, min, offset, meta.config.bytes_per_dim) < 0) {
            visit_left = false;
        }
    }

    if (visit_left) {
        if (auto visited = visit_node(tree, node.left, meta, data_in, min, max, visitor); !visited.has_value()) {
            return visited;
        }
    }
    if (visit_right) {
        if (auto visited = visit_node(tree, node.right, meta, data_in, min, max, visitor); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

} // namespace

VoidResult BkdTreeReader::intersect(const BkdIndexTree &tree, const BkdMeta &meta, IndexInput &data_in,
                                    std::span<const uint8_t> min, std::span<const uint8_t> max,
                                    const BkdPointVisitor &visitor) {
    const size_t index_bytes = meta.config.packed_index_bytes_length();
    if (min.size() != index_bytes || max.size() != index_bytes) {
        return err(LuceneError::illegal_argument("the query box must be packed_index_bytes_length() long"));
    }
    if (tree.nodes.empty()) {
        return err(LuceneError::illegal_state("the BKD index has no nodes"));
    }
    return visit_node(tree, 0, meta, data_in, min, max, visitor);
}

VoidResult BkdTreeReader::visit_all(const BkdIndexTree &tree, const BkdMeta &meta, IndexInput &data_in,
                                    const BkdPointVisitor &visitor) {
    if (tree.nodes.empty()) {
        return err(LuceneError::illegal_state("the BKD index has no nodes"));
    }
    return visit_node(tree, 0, meta, data_in, {}, {}, visitor);
}

} // namespace lucene
