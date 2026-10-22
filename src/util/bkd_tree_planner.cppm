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

export module lucene_core:bkd_tree_planner;

import :types;
import :error;

import std;

import :bkd_heap_point_writer;
import :bkd_radix_selector;

namespace lucene {

/// One leaf produced by [`BkdTreePlanner`]: a half-open point range.
export struct BkdLeaf {
    size_t from{0};
    size_t to{0};
    /// Dimension the block was ordered on (the parent's split dimension, or `0`
    /// for a tree that fits in a single leaf).
    ///
    /// The leaf encoder only uses this to pick which byte it run-length
    /// compresses, so any indexed dimension would be a legal choice.
    size_t sorted_dim{0};

    [[nodiscard]] size_t count() const noexcept { return to - from; }
};

/// One internal node of the planned tree.
export struct BkdSplit {
    /// Indexed dimension the node splits on.
    size_t dim{0};
    /// Split value, `bytes_per_dim` long.
    ///
    /// Bytes before the common prefix are zero-filled by [`BkdRadixSelector`],
    /// which is the form the packed index writer expects to prefix-code.
    std::vector<uint8_t> value;
};

/// The tree laid out in write order.
export struct BkdPlan {
    /// Leaves in the order their blocks must be written.
    std::vector<BkdLeaf> leaves;
    /// `splits[i]` is the node separating `leaves[i]` from `leaves[i + 1]`, so
    /// the array is `leaves.size() - 1` long and indexed by leaf boundary.
    ///
    /// That numbering is what the packed index uses: the on-disk format derives
    /// the tree shape from the leaf count (see [`BkdTreePlanner::num_left_leaf_nodes`])
    /// instead of storing it, so the planner and any reader must agree on it.
    std::vector<BkdSplit> splits;
};

/// Recursively splits the points of a [`HeapPointWriter`] until every block fits
/// into a leaf.
///
/// This is the recursive half of rlucene's `BKDWriter.build`
/// (`util/bkd/bkd_writer.rs`): pick a split dimension, partition the block in
/// place with [`BkdRadixSelector`], then recurse.  Producing the leaf ranges is
/// kept separate from encoding them, so the partitioning can be tested on its
/// own and the byte layout of leaves stays in the writer.
///
/// The shape is fixed: a subtree with `n` leaves gives
/// [`num_left_leaf_nodes`] leaves to its left child and splits the points at
/// `left_leaves * max_points_in_leaf`, exactly like rlucene.  The packed index
/// does not store the shape, so any other split would produce an index no
/// reader could walk.
///
/// **Deviation:** rlucene orders each leaf by the dimension whose next byte has
/// the fewest distinct values (to maximise run-length compression) and picks
/// split dimensions with an exact-bounds refresh every
/// `SPLITS_BEFORE_EXACT_BOUNDS` levels.  This port orders each leaf by the
/// dimension its parent split on and always derives the split dimension from a
/// scan of the block.  Both are compression choices: the bytes produced stay
/// valid, they are just occasionally larger.
export class BkdTreePlanner {
public:
    BkdTreePlanner(HeapPointWriter &writer, size_t max_points_in_leaf);

    /// Partitions the points and returns the leaves and the inner nodes in
    /// write order.
    ///
    /// A block that fits into `max_points_in_leaf` becomes a leaf.  A block that
    /// does not is always split, even when every indexed dimension is constant:
    /// the quota-based shape is part of the on-disk contract, so a degenerate
    /// (constant value) split is preferred over a shape no reader can derive.
    [[nodiscard]] Result<BkdPlan> plan();

    /// Just the leaves of [`plan`], for callers that do not build an index.
    [[nodiscard]] Result<std::vector<BkdLeaf>> build();

    /// The split dimension chosen for `[from, to)`, or `std::nullopt` when all
    /// indexed dimensions are constant.
    [[nodiscard]] std::optional<size_t> choose_split_dim(size_t from, size_t to) const;

    /// Leaves handed to the left child of a subtree with `num_leaves` leaves
    /// (rlucene's `get_num_left_leaf_nodes`).
    ///
    /// Unbalanced levels always go left, which is what keeps the subtree
    /// heights within one level of each other.
    [[nodiscard]] static size_t num_left_leaf_nodes(size_t num_leaves);

    /// Number of splits performed by the last [`plan`].
    [[nodiscard]] size_t split_count() const noexcept { return split_count_; }

    /// Maximum leaf size actually produced by the last [`plan`].
    [[nodiscard]] size_t max_leaf_count() const noexcept { return max_leaf_count_; }

private:
    [[nodiscard]] VoidResult build_range(size_t leaves_offset, size_t num_leaves, size_t from, size_t to,
                                          std::optional<size_t> sort_dim, BkdPlan &plan);

    HeapPointWriter *writer_;
    BkdRadixSelector selector_;
    size_t max_points_in_leaf_{0};
    size_t split_count_{0};
    size_t max_leaf_count_{0};
};

} // namespace lucene
