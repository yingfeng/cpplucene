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

export module lucene_core:merge_on_flush_merge_policy;

import :merge_policy;
import :merge_trigger;
import :segment_commit_info;
import :segment_infos;
import :types;

import std;

namespace lucene {

/// A policy that merges all small segments on a full flush, whatever the
/// delegate would do.
///
/// Port of `MergeOnFlushMergePolicy` (`sandbox/index/`).  On
/// [`MergeTrigger::FullFlush`] it asks for one merge of every segment smaller
/// than `small_segment_threshold_mb` (100 MB by default); every other question
/// goes to the delegate policy.
///
/// **Deviation:** the reference implementation splits full-flush merging into a
/// separate `find_full_flush_merges`; this port has one
/// [`MergePolicy::find_merges`] that receives the
/// [`MergeTrigger`](lucene_core:merge_trigger), so the override happens there.
export class MergeOnFlushMergePolicy final : public MergePolicy {
public:
    /// Takes ownership of `delegate` and installs the 100 MB default threshold.
    explicit MergeOnFlushMergePolicy(std::unique_ptr<MergePolicy> delegate);

    // ---- threshold ----

    /// The size under which a segment counts as small, in megabytes.
    [[nodiscard]] double small_segment_threshold_mb() const noexcept;
    /// Sets the size under which a segment counts as small, in megabytes.
    void set_small_segment_threshold_mb(double megabytes) noexcept;

    [[nodiscard]] std::string to_string() const override;

    // ---- MergePolicy ----

    [[nodiscard]] Result<std::optional<MergeSpecification>>
    find_merges(MergeTrigger merge_trigger, const SegmentInfos &infos,
                const MergeContext &context) const override;

    [[nodiscard]] Result<std::optional<MergeSpecification>>
    find_forced_merges(const SegmentInfos &infos, int32_t max_num_segments,
                       const MergeContext &context) const override;

    [[nodiscard]] Result<std::optional<MergeSpecification>>
    find_forced_deletes_merges(const SegmentInfos &infos, const MergeContext &context) const override;

    [[nodiscard]] bool use_compound_file(const SegmentInfos &infos, const SegmentCommitInfo &merged_info,
                                        const MergeContext &context) const override;

    [[nodiscard]] int64_t max_full_flush_merge_size() const override;

    // ---- introspection ----

    [[nodiscard]] MergePolicy &delegate() noexcept { return *delegate_; }
    [[nodiscard]] const MergePolicy &delegate() const noexcept { return *delegate_; }

private:
    /// The merge of every small, idle segment, or `nullopt` when there is at most
    /// one.
    [[nodiscard]] Result<std::optional<MergeSpecification>> small_segment_merge(const SegmentInfos &infos,
                                                                               const MergeContext &context) const;

    std::unique_ptr<MergePolicy> delegate_;
    int64_t small_segment_threshold_bytes_;
};

/// The `1024 * 1024` factor the threshold is expressed in.
export [[nodiscard]] inline double bytes_to_mb(int64_t bytes) noexcept {
    return static_cast<double>(bytes) / 1024.0 / 1024.0;
}

/// The byte count of `megabytes`.
export [[nodiscard]] inline int64_t mb_to_bytes(double megabytes) noexcept {
    return static_cast<int64_t>(megabytes * 1024.0 * 1024.0);
}

} // namespace lucene
