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

export module lucene_core:merge_policy;

import :types;
import std;

import :directory;
import :error;
import :info_stream;
import :merge_trigger;
import :segment_commit_info;
import :segment_infos;

namespace lucene {

/// If a merge's size is `>= 10%` of the index, compound files are disabled for
/// it.
export constexpr double DEFAULT_NO_CFS_RATIO = 0.1;
/// No upper bound on the size of a compound file segment.
export constexpr int64_t DEFAULT_MAX_CFS_SEGMENT_SIZE = std::numeric_limits<int64_t>::max();

/// What a [`MergePolicy`] may ask about the index while it plans merges.
///
/// Port of the `MergeContext` trait (`index/merge_policy.rs`).
///
/// **Deviations:** the reference implementation's context is built from `IndexWriter` internals
/// (`Inner`) and exposes them through `get_merging_segments(inner)`; this port
/// has no such handle, so [`get_merging_segments`] takes no argument and
/// [`directory`] is added because a segment's size can only be measured against
/// the directory that holds it (the reference implementation reads it from `SegmentCommitInfo`).
export class MergeContext {
public:
    virtual ~MergeContext() = default;

    /// Number of deletes a merge would claim back for `info`.
    [[nodiscard]] virtual Result<int32_t> num_deletes_to_merge(const SegmentCommitInfo &info) const = 0;

    /// Number of deleted documents in `info`, including pending deletes.
    [[nodiscard]] virtual Result<int32_t> num_deleted_docs(const SegmentCommitInfo &info) const = 0;

    /// Stream used for merge-policy logging.
    [[nodiscard]] virtual InfoStreamMT get_info_stream() const = 0;

    /// Names of the segments that are being merged right now.
    [[nodiscard]] virtual std::set<std::string> get_merging_segments() const = 0;

    /// Directory holding the segments, used to measure their size on disk.
    [[nodiscard]] virtual Directory *directory() const { return nullptr; }
};

/// The straightforward [`MergeContext`]: deletes are read from the commit info
/// and nothing is merging unless the caller says so.
export class SimpleMergeContext final : public MergeContext {
public:
    SimpleMergeContext(InfoStreamMT info_stream, Directory *directory);

    [[nodiscard]] Result<int32_t> num_deletes_to_merge(const SegmentCommitInfo &info) const override;
    [[nodiscard]] Result<int32_t> num_deleted_docs(const SegmentCommitInfo &info) const override;
    [[nodiscard]] InfoStreamMT get_info_stream() const override { return info_stream_; }
    [[nodiscard]] std::set<std::string> get_merging_segments() const override { return merging_; }
    [[nodiscard]] Directory *directory() const override { return directory_; }

    /// Marks `name` as being merged (or clears it).
    void set_merging(std::string name, bool merging);

private:
    InfoStreamMT info_stream_;
    Directory *directory_;
    std::set<std::string> merging_;
};

/// One primitive merge: the segments to combine into a single new one.
///
/// Port of `OneMerge` (`index/merge_policy.rs`), reduced to the data a policy
/// decides on: the reference implementation also carries the merge readers, the progress meter and
/// the rate limiter, which this port keeps in the merge executor instead.
export class OneMerge {
public:
    /// Creates a merge of `segments` (at least two).
    explicit OneMerge(std::vector<SegmentCommitInfo> segments);

    [[nodiscard]] const std::vector<SegmentCommitInfo> &segments() const noexcept { return segments_; }
    [[nodiscard]] size_t size() const noexcept { return segments_.size(); }

    /// Sum of the `max_doc` of the inputs.
    [[nodiscard]] int64_t total_max_doc() const noexcept;
    /// Sum of the live documents of the inputs.
    [[nodiscard]] int64_t total_doc_count(const MergeContext &context) const;

    /// `segment(id=1.2.3 maxDoc=10 delCount=1)` style description, one entry per
    /// input segment.
    [[nodiscard]] std::string to_string() const;

    /// The segment info the merge writes; the caller fills in the name and id.
    SegmentInfo merged_info;

private:
    std::vector<SegmentCommitInfo> segments_;
};

/// The set of merges a policy wants to run.
///
/// Port of `MergeSpecification`.
export class MergeSpecification {
public:
    MergeSpecification() = default;

    void add(OneMerge merge) { merges_.push_back(std::move(merge)); }

    [[nodiscard]] const std::vector<OneMerge> &merges() const noexcept { return merges_; }
    [[nodiscard]] size_t size() const noexcept { return merges_.size(); }
    [[nodiscard]] bool empty() const noexcept { return merges_.empty(); }

private:
    std::vector<OneMerge> merges_;
};

/// Decides which segments to merge.
///
/// Port of the `MergePolicy` trait (`index/merge_policy.rs`).
///
/// A policy never touches the index: it reads [`SegmentInfos`] and reports the
/// merges it wants, which the merge scheduler then executes.
export class MergePolicy {
public:
    virtual ~MergePolicy() = default;

    MergePolicy(const MergePolicy &) = delete;
    MergePolicy &operator=(const MergePolicy &) = delete;

    /// Name of the policy, for logging.
    [[nodiscard]] virtual std::string to_string() const = 0;

    /// The merges that are necessary now, or `nullopt` when none are.
    [[nodiscard]] virtual Result<std::optional<MergeSpecification>> find_merges(MergeTrigger merge_trigger,
                                                                                const SegmentInfos &infos,
                                                                                const MergeContext &context) const = 0;

    /// The merges necessary to leave at most `max_num_segments` segments.
    ///
    /// The default implementation reports no merge, which is what
    /// [`NoMergePolicy`] wants; the log policies override it.
    [[nodiscard]] virtual Result<std::optional<MergeSpecification>> find_forced_merges(
            const SegmentInfos &infos, int32_t max_num_segments, const MergeContext &context) const;

    /// The merges necessary to expunge all deletes.
    [[nodiscard]] virtual Result<std::optional<MergeSpecification>> find_forced_deletes_merges(
            const SegmentInfos &infos, const MergeContext &context) const;

    /// `true` when a new segment should be written as a compound file.
    ///
    /// **Deviation:** always `false`, because this port never writes `.cfs`
    /// files (see [`SegmentInfo::use_compound_file`]).
    [[nodiscard]] virtual bool use_compound_file(const SegmentInfos &infos, const SegmentCommitInfo &merged_info,
                                                 const MergeContext &context) const;

    /// Maximum size of the segments a full flush may merge synchronously; `0`
    /// disables full-flush merges.
    [[nodiscard]] virtual int64_t max_full_flush_merge_size() const { return 0; }

    /// `true` if `info` is already fully merged: no deletes and the compound
    /// file setting already matches this policy.
    [[nodiscard]] Result<bool> has_merged(const SegmentInfos &infos, const SegmentCommitInfo &info,
                                          const MergeContext &context) const;

    /// `true` when the policy logs to the context's info stream.
    [[nodiscard]] bool verbose() const noexcept { return verbose_; }
    void set_verbose(bool verbose) noexcept { verbose_ = verbose; }

    [[nodiscard]] double no_cfs_ratio() const noexcept { return no_cfs_ratio_; }
    [[nodiscard]] VoidResult set_no_cfs_ratio(double ratio);

    [[nodiscard]] int64_t max_cfs_segment_size() const noexcept { return max_cfs_segment_size_; }
    [[nodiscard]] double max_cfs_segment_size_mb() const noexcept;
    [[nodiscard]] VoidResult set_max_cfs_segment_size_mb(double megabytes);

    /// Description of the deletes of `infos`, used in log messages.
    [[nodiscard]] Result<std::string> seg_string(const MergeContext &context,
                                                 std::span<const SegmentCommitInfo> infos) const;

    /// Posts `message` to the context's info stream when [`verbose`] is set.
    void message(std::string_view message, const MergeContext &context) const;

protected:
    MergePolicy() = default;

    /// Number of deletes a merge of `info` would claim, as reported by
    /// [`MergeContext::num_deletes_to_merge`].
    [[nodiscard]] Result<int32_t> deletes_to_merge(const SegmentCommitInfo &info, const MergeContext &context) const;

private:
    bool verbose_{false};
    double no_cfs_ratio_{DEFAULT_NO_CFS_RATIO};
    int64_t max_cfs_segment_size_{DEFAULT_MAX_CFS_SEGMENT_SIZE};
};

/// A policy that never merges.  Port of `NoMergePolicy`.
export class NoMergePolicy final : public MergePolicy {
public:
    [[nodiscard]] static std::shared_ptr<NoMergePolicy> create();

    [[nodiscard]] std::string to_string() const override { return "NoMergePolicy"; }

    [[nodiscard]] Result<std::optional<MergeSpecification>> find_merges(MergeTrigger /*merge_trigger*/,
                                                                        const SegmentInfos & /*infos*/,
                                                                        const MergeContext & /*context*/) const override {
        return std::optional<MergeSpecification>{};
    }

    [[nodiscard]] Result<std::optional<MergeSpecification>> find_forced_merges(
            const SegmentInfos & /*infos*/, int32_t /*max_num_segments*/,
            const MergeContext & /*context*/) const override {
        return std::optional<MergeSpecification>{};
    }

    [[nodiscard]] Result<std::optional<MergeSpecification>> find_forced_deletes_merges(
            const SegmentInfos & /*infos*/, const MergeContext & /*context*/) const override {
        return std::optional<MergeSpecification>{};
    }
};

/// Bytes stored on disk for `info`, i.e. the sum of the lengths of its files.
///
/// Port of `SegmentCommitInfo::size_in_bytes`.
[[nodiscard]] Result<int64_t> segment_size_in_bytes(const SegmentCommitInfo &info, const MergeContext &context);

/// [`segment_size_in_bytes`] pro-rated by the deleted documents, which is the
/// size merge policies reason about: a segment full of deletes is cheap to
/// rewrite.
[[nodiscard]] Result<int64_t> calibrated_size_in_bytes(const SegmentCommitInfo &info, const MergeContext &context);

} // namespace lucene
