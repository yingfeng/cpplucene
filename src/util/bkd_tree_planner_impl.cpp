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

module lucene_core:bkd_tree_planner.impl;

import :bkd_tree_planner;

import :bkd_config;
import :bkd_heap_point_writer;
import :bkd_radix_selector;
import :bkd_util;
import :debug;
import :error;
import :types;

import std;

namespace lucene {

BkdTreePlanner::BkdTreePlanner(HeapPointWriter &writer, size_t max_points_in_leaf)
    : writer_(&writer), selector_(writer), max_points_in_leaf_(max_points_in_leaf) {
    debug_check(max_points_in_leaf > 0, "a leaf must hold at least one point");
}

size_t BkdTreePlanner::num_left_leaf_nodes(size_t num_leaves) {
    debug_check(num_leaves > 1, "a subtree with a single leaf has no children");

    // Fill the last complete level, then hand the leftovers to the left while
    // it stays at most twice the size of its sibling.
    size_t last_full_level = 0;
    for (size_t n = num_leaves; n > 1; n >>= 1) {
        ++last_full_level;
    }
    const size_t leaves_full_level = size_t{1} << last_full_level;
    size_t num_left_leaf_nodes = leaves_full_level / 2;
    const size_t unbalanced = num_leaves - leaves_full_level;
    num_left_leaf_nodes += std::min(unbalanced, num_left_leaf_nodes);
    return num_left_leaf_nodes;
}

std::optional<size_t> BkdTreePlanner::choose_split_dim(size_t from, size_t to) const {
    const auto &config = writer_->config();
    const size_t bytes_per_dim = config.bytes_per_dim;

    size_t best_dim = 0;
    size_t best_range = 0;
    bool any_range = false;

    for (size_t dim = 0; dim < config.num_index_dims; ++dim) {
        const size_t offset = dim * bytes_per_dim;
        // Track the min/max of this dimension over the block; the reference implementation does the
        // same scan while computing the leaf bounds.
        const auto first = writer_->packed_value(from).packed_value();
        std::vector<uint8_t> min_value(first.begin() + static_cast<std::ptrdiff_t>(offset),
                                       first.begin() + static_cast<std::ptrdiff_t>(offset + bytes_per_dim));
        std::vector<uint8_t> max_value = min_value;
        for (size_t i = from + 1; i < to; ++i) {
            const auto packed = writer_->packed_value(i).packed_value();
            if (BKDUtil::compare_unsigned(packed, offset, min_value, 0, bytes_per_dim) < 0) {
                std::copy_n(packed.begin() + static_cast<std::ptrdiff_t>(offset), bytes_per_dim, min_value.begin());
            } else if (BKDUtil::compare_unsigned(packed, offset, max_value, 0, bytes_per_dim) > 0) {
                std::copy_n(packed.begin() + static_cast<std::ptrdiff_t>(offset), bytes_per_dim, max_value.begin());
            }
        }
        if (BKDUtil::compare_unsigned(min_value, 0, max_value, 0, bytes_per_dim) == 0) {
            continue;
        }
        // "Range" is only used to rank dimensions, so the numeric distance
        // between the two bounds is enough.
        size_t range = 0;
        for (size_t b = 0; b < bytes_per_dim; ++b) {
            const size_t contribution =
                static_cast<size_t>(max_value[b] - min_value[b]) << (8 * (bytes_per_dim - 1 - b));
            range = range > contribution ? range : contribution;
        }
        if (!any_range || range > best_range) {
            any_range = true;
            best_range = range;
            best_dim = dim;
        }
    }

    if (!any_range) {
        return std::nullopt;
    }
    return best_dim;
}

VoidResult BkdTreePlanner::build_range(size_t leaves_offset, size_t num_leaves, size_t from, size_t to,
                                       std::optional<size_t> sort_dim, BkdPlan &plan) {
    if (num_leaves == 1) {
        // A leaf must be ordered for the run-length encoding to group equal
        // values; the parent's split dimension is the cheapest useful order.
        if (sort_dim.has_value() && to - from > 1) {
            if (auto sorted = selector_.sort(from, to, sort_dim.value() * writer_->config().bytes_per_dim);
                !sorted.has_value()) {
                return sorted;
            }
        }
        plan.leaves.push_back(BkdLeaf{from, to, sort_dim.value_or(0)});
        max_leaf_count_ = std::max(max_leaf_count_, to - from);
        return {};
    }

    const size_t num_left = num_left_leaf_nodes(num_leaves);
    const size_t left_count = num_left * max_points_in_leaf_;
    if (left_count == 0 || left_count >= to - from) {
        return err(LuceneError::illegal_state("the leaf quota does not fit the point range"));
    }

    const auto &config = writer_->config();
    const size_t split_dim = config.num_index_dims == 1 ? 0 : choose_split_dim(from, to).value_or(0);

    auto split_value = selector_.select(from, to, from + left_count, split_dim * config.bytes_per_dim);
    if (!split_value.has_value()) {
        return err(split_value.error());
    }
    ++split_count_;

    const size_t split_offset = leaves_offset + num_left - 1;
    debug_check(split_offset < plan.splits.size(), "the split index must be inside the plan");
    plan.splits[split_offset].dim = split_dim;
    plan.splits[split_offset].value = std::move(split_value.value());

    if (auto left = build_range(leaves_offset, num_left, from, from + left_count, split_dim, plan);
        !left.has_value()) {
        return left;
    }
    return build_range(leaves_offset + num_left, num_leaves - num_left, from + left_count, to, split_dim, plan);
}

Result<BkdPlan> BkdTreePlanner::plan() {
    if (!writer_->closed()) {
        return err(LuceneError::illegal_state("the point writer must be closed before planning"));
    }
    if (writer_->count() == 0) {
        return err(LuceneError::illegal_argument("cannot build a BKD tree without points"));
    }

    const size_t count = writer_->count();
    const size_t num_leaves = (count + max_points_in_leaf_ - 1) / max_points_in_leaf_;

    split_count_ = 0;
    max_leaf_count_ = 0;

    BkdPlan plan;
    plan.leaves.reserve(num_leaves);
    if (num_leaves > 1) {
        plan.splits.resize(num_leaves - 1);
    }
    if (auto built = build_range(0, num_leaves, 0, count, std::nullopt, plan); !built.has_value()) {
        return err(built.error());
    }
    return plan;
}

Result<std::vector<BkdLeaf>> BkdTreePlanner::build() {
    auto plan_result = plan();
    if (!plan_result.has_value()) {
        return err(plan_result.error());
    }
    return std::move(plan_result->leaves);
}

} // namespace lucene
