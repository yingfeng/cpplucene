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

export module lucene_core:bkd_writer;

import :types;
import :error;

import std;

import :bkd_config;
import :bkd_heap_point_writer;
import :bkd_offline_point;
import :bkd_tree_planner;
import :directory;
import :index_output;

namespace lucene {

/// What [`BkdWriter::finish`] produced, ready for [`BkdWriter::write_index`].
export struct BkdWriteResult {
    /// Points in the tree.
    size_t point_count{0};
    /// Distinct documents that contributed a point.
    int32_t doc_count{0};
    /// Points per leaf the tree was built with.
    size_t count_per_leaf{0};
    /// File pointer of the first leaf block (the caller's `data_out` position
    /// when `finish` started).
    size_t data_start_fp{0};
    /// File pointer of every leaf block, in leaf order.
    ///
    /// The packed index stores these as deltas, so they are also useful for
    /// tests and for callers that want to seek straight to a block.
    std::vector<size_t> leaf_block_fps;
    /// The packed index: split dimensions, prefix-coded split values and block
    /// deltas, in the byte layout `write_index` emits.
    std::vector<uint8_t> packed_index;
};

/// Builds a BKD tree and writes it to a point values file plus its index.
///
/// Port of `BKDWriter` (`util/bkd/bkd_writer.rs`).  The split shape, the leaf
/// layout and the packed index mirror the reference implementation; the differences are the ones
/// [`BkdTreePlanner`] and [`BkdLeafWriter`] already document (leaf ordering,
/// split-dimension choice).
///
/// **Spilling:** when a [`Directory`] and a heap budget are passed to
/// [`create`], points beyond `max_points_in_heap` are appended to a temporary
/// [`OfflinePointWriter`] instead of memory, and `finish` builds the tree with
/// a bounded working set: each partition is a radix pass over the spilled file,
/// and each leaf is materialised into a heap block of at most
/// `max_points_in_leaf` points.  Peak memory is therefore O(maxPointsInLeaf)
/// instead of O(totalPointCount).  The reference implementation decides per `finish` whether to
/// spill; here the decision is made up front from `total_point_count`, which is
/// known at construction time.
export class BkdWriter {
public:
    /// In-memory build.  The whole point set is held in one heap block.
    [[nodiscard]] static Result<BkdWriter> create(const BKDConfig &config, size_t total_point_count, int32_t max_doc);

    /// Build that spills to `dir` once more than `max_points_in_heap` points
    /// were added.
    ///
    /// `max_points_in_heap == 0` keeps everything in memory.  `dir` must
    /// outlive the writer; temporary files are deleted by [`close`] and by the
    /// destructor.
    [[nodiscard]] static Result<BkdWriter> create(const BKDConfig &config, size_t total_point_count, int32_t max_doc,
                                                  Directory *dir, size_t max_points_in_heap);

    ~BkdWriter();

    BkdWriter(const BkdWriter &) = delete;
    BkdWriter &operator=(const BkdWriter &) = delete;
    BkdWriter(BkdWriter &&) noexcept;
    BkdWriter &operator=(BkdWriter &&) noexcept;

    /// Appends one point.  `packed_value` must be `packed_bytes_length()` long.
    [[nodiscard]] VoidResult add(std::span<const uint8_t> packed_value, int32_t doc_id);

    /// Writes every leaf block into `data_out` and packs the index.
    ///
    /// Returns an empty result (no leaves) when no point was added, matching
    /// The reference implementation's `finish` returning `None`.  Calling it twice fails.
    [[nodiscard]] Result<BkdWriteResult> finish(IndexOutput &data_out);

    /// Writes the `.kdm`/`.kdi` pair for a finished tree.
    ///
    /// `index_out` may be `nullptr`, in which case the packed index is appended
    /// to `meta_out` (the reference implementation's single-file layout).  The index file pointer is
    /// recorded in the meta file either way.
    [[nodiscard]] VoidResult write_index(IndexOutput &meta_out, IndexOutput *index_out,
                                         const BkdWriteResult &data);

    [[nodiscard]] size_t point_count() const noexcept { return point_count_; }
    [[nodiscard]] int32_t doc_count() const noexcept { return doc_count_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    /// `true` when the points went to the temporary file instead of memory.
    [[nodiscard]] bool spilled() const noexcept { return offline_writer_ != nullptr; }
    [[nodiscard]] const BKDConfig &config() const noexcept { return config_; }

    /// Smallest/largest value seen per indexed dimension, `packed_index_bytes_length()`
    /// long each.  Only meaningful once at least one point was added.
    [[nodiscard]] std::span<const uint8_t> min_packed_value() const noexcept { return min_packed_value_; }
    [[nodiscard]] std::span<const uint8_t> max_packed_value() const noexcept { return max_packed_value_; }

    /// Releases the point buffers and deletes any temporary file.
    [[nodiscard]] VoidResult close();

private:
    BkdWriter(const BKDConfig &config, size_t total_point_count, int32_t max_doc, Directory *dir,
              size_t max_points_in_heap);

    /// Heap build: partitions in place and encodes the planned leaves.
    [[nodiscard]] Result<BkdWriteResult> finish_on_heap(IndexOutput &data_out);

    BKDConfig config_;
    /// Exactly one of the two is set, decided by `total_point_count` versus the
    /// heap budget.
    std::unique_ptr<HeapPointWriter> heap_writer_;
    std::unique_ptr<OfflinePointWriter> offline_writer_;
    Directory *dir_{nullptr};
    size_t max_points_in_heap_{0};
    size_t total_point_count_{0};
    size_t point_count_{0};
    int32_t max_doc_{0};
    int32_t doc_count_{0};
    std::vector<uint8_t> docs_seen_;
    std::vector<uint8_t> min_packed_value_;
    std::vector<uint8_t> max_packed_value_;
    bool finished_{false};
    bool closed_{false};
};

} // namespace lucene
