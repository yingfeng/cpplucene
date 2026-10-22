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

export module lucene_core:bkd_leaf_writer;

import :types;
import :error;

import std;

import :bkd_heap_point_writer;
import :bkd_tree_planner;
import :data_output;

namespace lucene {

/// What a leaf block turned out to look like, for the caller's bookkeeping.
export struct BkdLeafBlockInfo {
    /// Points in this block.
    size_t count{0};
    /// Per dimension common prefix length that was written (`num_dims` entries).
    std::vector<size_t> common_prefix_lengths;
    /// Number of runs of equal values (`1` means "all equal").
    ///
    /// This is what the block actually stores; it equals the number of distinct
    /// values whenever the block is ordered.
    size_t cardinality{0};
    /// Dimension whose byte was run-length compressed, when the high-cardinality
    /// form was chosen.
    size_t sorted_dim{0};
};

/// Encodes one leaf block of a BKD tree.
///
/// Port of `write_leaf_block_*` in rlucene's `BKDWriter`
/// (`util/bkd/bkd_writer.rs`).  A block is written as:
///
/// ```text
/// vint(count)                       // doc ids
/// docIds                            // BkdDocIdsWriter encoded
/// for each dim: vint(prefix) + prefix bytes
/// packed values:
///   0xFF                            // every value equal
///   0xFE                            // low cardinality, see below
///   0..numDims-1                    // high cardinality: the marker is the
///                                   // compressed dimension, see below
/// ```
///
/// The `0xFE` form is a sequence of `(vint runLength, suffix)` pairs, one pair
/// per run of equal values, where `suffix` is the value with the
/// per-dimension common prefix stripped.  Equal values are adjacent because the
/// block is sorted on one dimension before it is encoded, so `runLength` is the
/// number of points carrying that value.
///
/// The high-cardinality form is a sequence of `(bucket byte, runLength byte,
/// suffix...)` groups: the block is split into runs that agree on the compressed
/// byte, and that byte is stored once per run instead of once per point, so the
/// per-point suffix is one byte shorter.  It wins whenever the block has few
/// repeated values, which is the common case for id-like fields.
///
/// When more than one dimension is indexed, `min`/`max` suffixes for every
/// indexed dimension are written first (the same "actual bounds" rlucene emits)
/// so a reader can prune the block without materialising it.
///
/// All three forms rlucene produces are written: the encoder costs the two
/// variable-length layouts first and picks the smaller one.  A block of distinct
/// values always ends up in the run-length form (two bytes per run beats one byte
/// plus a suffix per value), and a block with a few repeated values ends up in the
/// low-cardinality form.
export struct BkdLeafWriter {
    /// Writes `leaf` (a range inside `points`) into `out`.
    ///
    /// `points` must be closed.  The common prefixes are computed by scanning
    /// the whole range, so the block does not have to be ordered for the result
    /// to be correct; ordering it on one dimension (as [`BkdTreePlanner`] does)
    /// only makes the run-length encoding more effective.  The caller owns the
    /// file pointer bookkeeping: record `IndexOutput::get_file_pointer()` before
    /// the call if the block offsets are needed later.
    [[nodiscard]] static Result<BkdLeafBlockInfo> write_leaf_block(HeapPointWriter &points, const BkdLeaf &leaf,
                                                                   DataOutput &out);
};

} // namespace lucene
