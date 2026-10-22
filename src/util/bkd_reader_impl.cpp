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

module lucene_core:bkd_reader.impl;

import :bkd_reader;

import :bkd_config;
import :bkd_index_tree;
import :bkd_leaf_reader;
import :bytes_ref;
import :error;
import :index_input;
import :point_values;
import :types;

import std;

namespace lucene {

namespace {

[[nodiscard]] LuceneError corrupt(std::string message) {
    return LuceneError::corrupt_index(std::move(message));
}

/// Unsigned lexicographic comparison of two `bytes_per_dim` slices.
[[nodiscard]] int32_t compare_dim(std::span<const uint8_t> left, std::span<const uint8_t> right, size_t bytes_per_dim) {
    for (size_t i = 0; i < bytes_per_dim; ++i) {
        if (left[i] != right[i]) {
            return left[i] < right[i] ? -1 : 1;
        }
    }
    return 0;
}

} // namespace

BkdPointTree::BkdPointTree(std::shared_ptr<const BkdMeta> meta, std::shared_ptr<const BkdIndexTree> index,
                           std::unique_ptr<IndexInput> data_in)
    : meta_(std::move(meta)), index_(std::move(index)), data_in_(std::move(data_in)) {}

Result<std::unique_ptr<BkdPointTree>> BkdPointTree::create(std::shared_ptr<const BkdMeta> meta,
                                                           std::shared_ptr<const BkdIndexTree> index,
                                                           std::unique_ptr<IndexInput> data_in) {
    if (meta == nullptr || index == nullptr || data_in == nullptr) {
        return err(LuceneError::illegal_argument("a BKD point tree needs a meta block, an index and a data input"));
    }
    if (index->nodes.empty()) {
        return err(corrupt("the BKD index has no nodes"));
    }
    // `make_unique` cannot reach the private constructor, so build the pointer here.
    return std::unique_ptr<BkdPointTree>(new BkdPointTree(std::move(meta), std::move(index), std::move(data_in)));
}

bool BkdPointTree::is_leaf() const noexcept {
    return index_->nodes[node_index_].leaf;
}

Result<bool> BkdPointTree::move_to_child() {
    if (is_leaf()) {
        return false;
    }
    node_index_ = node().left;
    return true;
}

Result<bool> BkdPointTree::move_to_sibling() {
    const BkdIndexNode &current = node();
    if (current.parent == kNoBkdParent || !current.is_left) {
        return false;
    }
    node_index_ = index_->nodes[current.parent].right;
    return true;
}

Result<bool> BkdPointTree::move_to_parent() {
    const BkdIndexNode &current = node();
    if (current.parent == kNoBkdParent) {
        return false;
    }
    node_index_ = current.parent;
    return true;
}

Result<BytesRef> BkdPointTree::get_min_packed_value() {
    return BytesRef(node().min_value);
}

Result<BytesRef> BkdPointTree::get_max_packed_value() {
    return BytesRef(node().max_value);
}

size_t BkdPointTree::leaves_before() const {
    size_t count = 0;
    size_t current = node_index_;
    while (true) {
        const BkdIndexNode &node_here = index_->nodes[current];
        if (node_here.parent == kNoBkdParent) {
            break;
        }
        if (!node_here.is_left) {
            count += index_->nodes[index_->nodes[node_here.parent].left].num_leaves;
        }
        current = node_here.parent;
    }
    return count;
}

Result<size_t> BkdPointTree::size() {
    const BkdIndexNode &current = node();
    const size_t max_per_leaf = meta_->config.max_points_in_leaf_node;
    const size_t total_leaves = index_->num_leaves();
    if (current.num_leaves == 0 || max_per_leaf == 0) {
        return size_t{0};
    }

    const size_t last_leaf = leaves_before() + current.num_leaves - 1;
    if (last_leaf == total_leaves - 1) {
        // The subtree contains the tree's last leaf, which holds the remainder of
        // the points instead of a full block.
        const auto full_leaves = static_cast<int64_t>(total_leaves - 1);
        const int64_t in_full_leaves = full_leaves * static_cast<int64_t>(max_per_leaf);
        if (meta_->point_count >= in_full_leaves) {
            const auto last_leaf_points = meta_->point_count - in_full_leaves;
            const auto size = static_cast<int64_t>(current.num_leaves - 1) * static_cast<int64_t>(max_per_leaf) +
                              last_leaf_points;
            return static_cast<size_t>(size);
        }
    }
    return current.num_leaves * max_per_leaf;
}

VoidResult BkdPointTree::visit_subtree(IntersectVisitor &visitor, bool with_values) {
    std::vector<size_t> stack{node_index_};
    while (!stack.empty()) {
        const size_t index_here = stack.back();
        stack.pop_back();
        const BkdIndexNode &node_here = index_->nodes[index_here];
        if (!node_here.leaf) {
            // Push right first so the left subtree is visited first, which keeps
            // the traversal in leaf order.
            stack.push_back(node_here.right);
            stack.push_back(node_here.left);
            continue;
        }

        auto leaf = BkdLeafReader::read_leaf_block(*data_in_, meta_->config, node_here.leaf_block_fp);
        if (!leaf.has_value()) {
            return std::unexpected(std::move(leaf.error()));
        }
        const BkdDecodedLeaf &decoded = leaf.value();
        if (decoded.doc_ids.size() != decoded.values.size()) {
            return err(corrupt("the decoded leaf block has mismatched document ids and values"));
        }
        for (size_t i = 0; i < decoded.doc_ids.size(); ++i) {
            if (with_values) {
                const std::span<const uint8_t> packed(decoded.values[i]);
                if (auto visited = visitor.visit_with_packed_value(decoded.doc_ids[i], packed); !visited.has_value()) {
                    return visited;
                }
            } else {
                if (auto visited = visitor.visit(decoded.doc_ids[i]); !visited.has_value()) {
                    return visited;
                }
            }
        }
    }
    return {};
}

VoidResult BkdPointTree::visit_doc_ids(IntersectVisitor &visitor) {
    return visit_subtree(visitor, /*with_values=*/false);
}

VoidResult BkdPointTree::visit_doc_values(IntersectVisitor &visitor) {
    return visit_subtree(visitor, /*with_values=*/true);
}

std::unique_ptr<PointTree> BkdPointTree::try_clone() const {
    auto cloned_data = data_in_->clone();
    if (!cloned_data.has_value()) {
        return nullptr;
    }
    auto cursor = create(meta_, index_, std::move(cloned_data.value()));
    if (!cursor.has_value()) {
        return nullptr;
    }
    auto result = std::move(cursor.value());
    result->node_index_ = node_index_;
    return result;
}

BkdReader::BkdReader(std::shared_ptr<const BkdMeta> meta, std::shared_ptr<const BkdIndexTree> index,
                     std::unique_ptr<IndexInput> data_in)
    : meta_(std::move(meta)), index_(std::move(index)), data_in_(std::move(data_in)) {}

Result<std::shared_ptr<BkdReader>> BkdReader::open(std::shared_ptr<const BkdMeta> meta,
                                                   std::shared_ptr<const BkdIndexTree> index,
                                                   std::unique_ptr<IndexInput> data_in) {
    if (meta == nullptr || index == nullptr || data_in == nullptr) {
        return err(LuceneError::illegal_argument("a BKD reader needs a meta block, an index and a data input"));
    }
    if (index->nodes.empty()) {
        return err(corrupt("the BKD index has no nodes"));
    }
    if (index->num_leaves() != meta->num_leaves) {
        return err(corrupt("the index and the meta block disagree on the leaf count"));
    }

    // The reference implementation validates the stored bounds here; a crossed box means the tree
    // cannot be traversed sensibly, so reject it while the reader is built.
    const size_t bytes_per_dim = meta->config.bytes_per_dim;
    for (size_t dim = 0; dim < meta->config.num_index_dims; ++dim) {
        const size_t offset = dim * bytes_per_dim;
        if (compare_dim(std::span<const uint8_t>(meta->min_value).subspan(offset, bytes_per_dim),
                        std::span<const uint8_t>(meta->max_value).subspan(offset, bytes_per_dim),
                        bytes_per_dim) > 0) {
            return err(corrupt("the minimum packed value is greater than the maximum packed value in dimension " +
                               std::to_string(dim)));
        }
    }

    return std::shared_ptr<BkdReader>(new BkdReader(std::move(meta), std::move(index), std::move(data_in)));
}

Result<std::optional<BytesRef>> BkdReader::get_min_packed_value() const {
    return std::optional<BytesRef>(BytesRef(meta_->min_value));
}

Result<std::optional<BytesRef>> BkdReader::get_max_packed_value() const {
    return std::optional<BytesRef>(BytesRef(meta_->max_value));
}

Result<size_t> BkdReader::get_num_dimensions() const {
    return meta_->config.num_dims;
}

Result<size_t> BkdReader::get_num_index_dimensions() const {
    return meta_->config.num_index_dims;
}

Result<size_t> BkdReader::get_bytes_per_dimension() const {
    return meta_->config.bytes_per_dim;
}

Result<size_t> BkdReader::size() const {
    return static_cast<size_t>(meta_->point_count);
}

Result<int32_t> BkdReader::get_doc_count() const {
    return meta_->doc_count;
}

Result<std::unique_ptr<PointTree>> BkdReader::get_point_tree() const {
    auto cloned_data = data_in_->clone();
    if (!cloned_data.has_value()) {
        return std::unexpected(std::move(cloned_data.error()));
    }
    auto cursor = BkdPointTree::create(meta_, index_, std::move(cloned_data.value()));
    if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
    }
    return std::unique_ptr<PointTree>(std::move(cursor.value()));
}

} // namespace lucene
