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

export module lucene_core:log_merge_policy;

import :types;
import std;

import :error;
import :merge_policy;
import :merge_trigger;
import :segment_commit_info;
import :segment_infos;

namespace lucene {

/// Range of `log(size)` covered by one level.
export constexpr double LEVEL_LOG_SPAN = 0.75;
/// How many segments are merged at a time.
export constexpr int32_t DEFAULT_MERGE_FACTOR = 10;
/// Segments with more documents are never merged.
export constexpr int32_t DEFAULT_MAX_MERGE_DOCS = std::numeric_limits<int32_t>::max();

/// Merges segments into levels of exponentially increasing size, where each
/// level has fewer segments than the merge factor.
///
/// Port of `LogMergePolicy` (`index/log_merge_policy.rs`).  The concrete size
/// measure (documents or bytes) is supplied by [`LogDocMergePolicy`] and
/// [`LogByteSizeMergePolicy`] through [`size`].
///
/// **Deviation:** the reference implementation parameterises this class over a `LogMergePolicyBase`
/// sub-type and passes the `IndexWriter`'s segment-eligibility map to
/// `find_forced_merges`; here the size measure is a virtual method and forced
/// merges treat every segment as eligible (the writer has no
/// `force_merge` snapshot yet).
export class LogMergePolicy : public MergePolicy {
public:
    [[nodiscard]] int32_t merge_factor() const noexcept { return merge_factor_; }
    [[nodiscard]] VoidResult set_merge_factor(int32_t merge_factor);

    /// Segments smaller than this are candidates for full-flush merges.
    [[nodiscard]] int64_t min_merge_size() const noexcept { return min_merge_size_; }
    /// Used by [`LogDocMergePolicy::set_min_merge_docs`].
    void set_min_merge_size(int64_t min_merge_size) noexcept { min_merge_size_ = min_merge_size; }
    [[nodiscard]] double min_merge_mb() const noexcept;
    [[nodiscard]] VoidResult set_min_merge_mb(double min_merge_mb);

    /// Segments larger than this are never merged.
    [[nodiscard]] int64_t max_merge_size() const noexcept { return max_merge_size_; }
    void set_max_merge_size(int64_t max_merge_size) noexcept { max_merge_size_ = max_merge_size; }
    [[nodiscard]] double max_merge_mb() const noexcept;
    [[nodiscard]] VoidResult set_max_merge_mb(double max_merge_mb);

    /// Segments larger than this are never merged by a forced merge.
    [[nodiscard]] int64_t max_merge_size_for_forced_merge() const noexcept { return max_merge_size_for_forced_merge_; }
    void set_max_merge_size_for_forced_merge(int64_t size) noexcept { max_merge_size_for_forced_merge_ = size; }
    [[nodiscard]] double max_merge_mb_for_forced_merge() const noexcept;
    [[nodiscard]] VoidResult set_max_merge_mb_for_forced_merge(double max_merge_mb);

    /// Segments with more documents than this are never merged.
    [[nodiscard]] int32_t max_merge_docs() const noexcept { return max_merge_docs_; }
    void set_max_merge_docs(int32_t max_merge_docs) noexcept { max_merge_docs_ = max_merge_docs; }

    /// Whether a segment's size is prorated by its live documents.
    [[nodiscard]] bool calibrate_size_by_deletes() const noexcept { return calibrate_size_by_deletes_; }
    void set_calibrate_size_by_deletes(bool calibrate) noexcept { calibrate_size_by_deletes_ = calibrate; }

    /// Prevents merges from creating segments bigger than `max_doc / target`.
    [[nodiscard]] int32_t target_search_concurrency() const noexcept { return target_search_concurrency_; }
    [[nodiscard]] VoidResult set_target_search_concurrency(int32_t target_search_concurrency);

    // ---- MergePolicy ----

    [[nodiscard]] Result<std::optional<MergeSpecification>> find_merges(MergeTrigger merge_trigger,
                                                                        const SegmentInfos &infos,
                                                                        const MergeContext &context) const override;

    [[nodiscard]] Result<std::optional<MergeSpecification>> find_forced_merges(
            const SegmentInfos &infos, int32_t max_num_segments, const MergeContext &context) const override;

    [[nodiscard]] Result<std::optional<MergeSpecification>> find_forced_deletes_merges(
            const SegmentInfos &infos, const MergeContext &context) const override;

    [[nodiscard]] int64_t max_full_flush_merge_size() const override { return max_merge_size_; }

protected:
    LogMergePolicy() = default;

    /// The size of `info` as this policy measures it (documents or bytes),
    /// prorated by the live documents when calibration is on.
    [[nodiscard]] virtual Result<int64_t> size(const SegmentCommitInfo &info,
                                               const MergeContext &context) const = 0;

    /// Documents of `info`, prorated by the live documents when calibration is
    /// on.  Port of `size_docs`.
    [[nodiscard]] Result<int64_t> size_docs(const SegmentCommitInfo &info, const MergeContext &context) const;

    /// Byte size of `info`, prorated by the live documents when calibration is
    /// on.  Port of `size_bytes`.
    [[nodiscard]] Result<int64_t> size_bytes(const SegmentCommitInfo &info, const MergeContext &context) const;

    /// `true` when at most `max_num_segments` segments would remain.
    [[nodiscard]] Result<bool> is_merged(const SegmentInfos &infos, int32_t max_num_segments,
                                         const MergeContext &context) const;

    int32_t merge_factor_{DEFAULT_MERGE_FACTOR};
    int64_t min_merge_size_{0};
    int64_t max_merge_size_{0};
    int64_t max_merge_size_for_forced_merge_{std::numeric_limits<int64_t>::max()};
    int32_t max_merge_docs_{DEFAULT_MAX_MERGE_DOCS};
    bool calibrate_size_by_deletes_{true};
    int32_t target_search_concurrency_{1};

private:
    /// Splits `[start, last)` with the max-merge-size rule.  Port of
    /// `find_forced_merges_size_limit`.
    [[nodiscard]] Result<std::optional<MergeSpecification>> find_forced_merges_size_limit(
            const SegmentInfos &infos, int32_t last, const MergeContext &context) const;

    /// Splits `[0, last)` so that `max_num_segments` remain.  Port of
    /// `find_forced_merges_max_num_segments`.
    [[nodiscard]] Result<std::optional<MergeSpecification>> find_forced_merges_max_num_segments(
            const SegmentInfos &infos, int32_t max_num_segments, int32_t last, const MergeContext &context) const;
};

/// A [`LogMergePolicy`] that measures a segment by its document count.
///
/// Port of `LogDocMergePolicy`.
export class LogDocMergePolicy final : public LogMergePolicy {
public:
    /// Segments with at most this many documents get merged more aggressively.
    static constexpr int32_t DEFAULT_MIN_MERGE_DOCS = 1000;

    /// A policy with Lucene's defaults (`merge_factor=10`, `min_merge_docs=1000`,
    /// no maximum size).
    [[nodiscard]] static std::unique_ptr<LogDocMergePolicy> create();

    [[nodiscard]] std::string to_string() const override;

    [[nodiscard]] int32_t min_merge_docs() const noexcept { return static_cast<int32_t>(min_merge_size_); }
    void set_min_merge_docs(int32_t min_merge_docs) noexcept { set_min_merge_size(min_merge_docs); }

protected:
    [[nodiscard]] Result<int64_t> size(const SegmentCommitInfo &info, const MergeContext &context) const override;
};

/// A [`LogMergePolicy`] that measures a segment by the total size of its files.
///
/// Port of `LogByteSizeMergePolicy`.
///
/// **Deviation:** the size is read through [`MergeContext::directory`], so the
/// context must be built with a directory (see [`SimpleMergeContext`]).
export class LogByteSizeMergePolicy final : public LogMergePolicy {
public:
    /// Segments smaller than this are candidates for full-flush merges.
    static constexpr double DEFAULT_MIN_MERGE_MB = 1.6;
    /// Segments larger than this are never merged.
    static constexpr double DEFAULT_MAX_MERGE_MB = 2048.0;

    [[nodiscard]] static std::unique_ptr<LogByteSizeMergePolicy> create();

    [[nodiscard]] std::string to_string() const override;

protected:
    [[nodiscard]] Result<int64_t> size(const SegmentCommitInfo &info, const MergeContext &context) const override;
};

} // namespace lucene
