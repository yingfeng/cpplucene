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

export module lucene_core:tiered_merge_policy;

import :types;
import std;

import :error;
import :merge_policy;
import :merge_trigger;
import :segment_commit_info;
import :segment_infos;

namespace lucene {

/// Segments merged at a time during normal merging.
export constexpr int32_t DEFAULT_MAX_MERGE_AT_ONCE = 10;
/// Segments a tier may hold before it is over budget.
export constexpr double DEFAULT_SEGS_PER_TIER = 10.0;
/// Largest segment normal merging may produce: 5 GB.
export constexpr int64_t DEFAULT_MAX_MERGED_SEGMENT_BYTES = 5LL * 1024 * 1024 * 1024;
/// Segments below this size are merged aggressively: 2 MB.
export constexpr int64_t DEFAULT_FLOOR_SEGMENT_BYTES = 2LL * 1024 * 1024;
/// Percentage of deletes a segment may keep before it is worth rewriting.
export constexpr double DEFAULT_DELETES_PCT_ALLOWED = 20.0;
/// Percentage of deletes that makes a segment worth rewriting in a forced
/// deletes merge.
export constexpr double DEFAULT_FORCE_MERGE_DELETES_PCT_ALLOWED = 10.0;

/// Merges segments of approximately equal size, subject to an allowed number of
/// segments per tier.
///
/// Port of `TieredMergePolicy` (`index/tiered_merge_policy.rs`), the reference
/// implementation's default policy.  Unlike [`LogMergePolicy`] it merges
/// non-adjacent segments, separates how many segments are merged at once
/// (`max_merge_at_once`) from how many are allowed per tier (`segments_per_tier`),
/// and never over-merges (no cascading).
///
/// Merging first computes a budget of how many segments the index may hold.  Over
/// budget, segments are sorted by decreasing size (pro-rated by percent deletes)
/// and the cheapest merge is picked, where "cheap" combines the skew of the merge
/// (largest input over smallest), its total size and the deletes it reclaims.
///
/// Sizes are always measured in bytes and always pro-rated by the deleted
/// documents, so the context must have a [`directory`](MergeContext::directory).
///
/// **Deviation:** the reference implementation passes the `IndexWriter`'s
/// segment-eligibility map to `find_forced_merges` and treats non-original
/// segments differently; this port has no such map yet and treats every segment
/// as eligible for a forced merge.
export class TieredMergePolicy final : public MergePolicy {
public:
    [[nodiscard]] static std::unique_ptr<TieredMergePolicy> create();

    [[nodiscard]] std::string to_string() const override;

    /// Maximum number of segments merged at a time; at least 2, default 10.
    [[nodiscard]] int32_t max_merge_at_once() const noexcept { return max_merge_at_once_; }
    [[nodiscard]] VoidResult set_max_merge_at_once(int32_t value);

    /// Largest segment normal merging may produce, in megabytes (default 5120).
    [[nodiscard]] double max_merged_segment_mb() const noexcept;
    [[nodiscard]] VoidResult set_max_merged_segment_mb(double value);

    /// Segments below this size, in megabytes, are merged aggressively
    /// (default 2).
    [[nodiscard]] double floor_segment_mb() const noexcept;
    [[nodiscard]] VoidResult set_floor_segment_mb(double value);

    /// Segments allowed per tier; must be greater than 1 (default 10).
    [[nodiscard]] double segments_per_tier() const noexcept { return segments_per_tier_; }
    [[nodiscard]] VoidResult set_segments_per_tier(double value);

    /// Percentage of deletes normal merging tolerates (default 20).
    [[nodiscard]] double deletes_pct_allowed() const noexcept { return deletes_pct_allowed_; }
    [[nodiscard]] VoidResult set_deletes_pct_allowed(double value);

    /// Percentage of deletes a forced deletes merge reclaims (default 10).
    [[nodiscard]] double force_merge_deletes_pct_allowed() const noexcept {
        return force_merge_deletes_pct_allowed_;
    }
    [[nodiscard]] VoidResult set_force_merge_deletes_pct_allowed(double value);

    /// Keeps at least this many segments so a search can split its work
    /// (default 1).
    [[nodiscard]] int32_t target_search_concurrency() const noexcept { return target_search_concurrency_; }
    [[nodiscard]] VoidResult set_target_search_concurrency(int32_t value);

    // ---- MergePolicy ----

    [[nodiscard]] Result<std::optional<MergeSpecification>> find_merges(MergeTrigger merge_trigger,
                                                                        const SegmentInfos &infos,
                                                                        const MergeContext &context) const override;

    [[nodiscard]] Result<std::optional<MergeSpecification>> find_forced_merges(
            const SegmentInfos &infos, int32_t max_num_segments, const MergeContext &context) const override;

    [[nodiscard]] Result<std::optional<MergeSpecification>> find_forced_deletes_merges(
            const SegmentInfos &infos, const MergeContext &context) const override;

    /// Segments below the floor size are candidates for full-flush merges.
    [[nodiscard]] int64_t max_full_flush_merge_size() const override { return floor_segment_bytes_; }

private:
    TieredMergePolicy();

    /// A segment with the two sizes the policy needs: the pro-rated one used for
    /// the budget, and the raw one used for the merge-size limits.
    struct SegmentSizeAndDocs {
        const SegmentCommitInfo *seg_info;
        int64_t size_in_bytes;
        int64_t size_in_seg;
        int32_t del_count;
        int32_t max_doc;
    };

    enum class MergeType : uint8_t {
        /// Normal merging.
        Natural = 0,
        /// `IndexWriter::force_merge`.
        ForceMerge = 1,
        /// `IndexWriter::force_merge_deletes`.
        ForceMergeDeletes = 2,
    };

    /// The cost of one candidate merge; lower is better.
    struct MergeScore {
        double final_merge_score;
        double skew;
        double non_del_ratio;

        [[nodiscard]] double score() const noexcept { return final_merge_score; }
        [[nodiscard]] std::string explanation() const;
    };

    /// The segments of `infos`, largest first (ties broken by name).
    [[nodiscard]] Result<std::vector<SegmentSizeAndDocs>> get_sorted_by_segment_size(
            const SegmentInfos &infos, const MergeContext &context) const;

    /// Picks merges among `sorted_eligible` until the index is within budget.
    [[nodiscard]] Result<std::optional<MergeSpecification>> do_find_merges(
            std::vector<SegmentSizeAndDocs> sorted_eligible, int64_t max_merged_segment_bytes, int32_t merge_factor,
            size_t allowed_seg_count, int32_t allowed_del_count, int32_t allowed_doc_count, MergeType merge_type,
            const MergeContext &context, bool max_merge_is_running) const;

    /// [`MergeScore`] of a candidate merge.
    [[nodiscard]] Result<MergeScore> score(std::span<const SegmentSizeAndDocs> candidate, bool hit_too_large,
                                           const std::map<std::string, SegmentSizeAndDocs> &segments_sizes) const;

    /// Documents a merge may cover, given the target search concurrency.
    [[nodiscard]] int32_t get_max_allowed_docs(int32_t total_max_doc, int32_t total_del_docs) const;

    /// The pro-rated size of `bytes`, never below the floor.
    [[nodiscard]] int64_t floor_size(int64_t bytes) const;

    int32_t max_merge_at_once_;
    int64_t max_merged_segment_bytes_;
    int64_t floor_segment_bytes_;
    double segments_per_tier_;
    double force_merge_deletes_pct_allowed_;
    double deletes_pct_allowed_;
    int32_t target_search_concurrency_;
};

} // namespace lucene
