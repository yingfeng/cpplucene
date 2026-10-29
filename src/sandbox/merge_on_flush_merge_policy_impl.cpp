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

module lucene_core:merge_on_flush_merge_policy.impl;

import :merge_on_flush_merge_policy;

import :error;
import :merge_policy;
import :merge_trigger;
import :segment_commit_info;
import :segment_infos;
import :types;

import std;

namespace lucene {

namespace {
constexpr int64_t kDefaultSmallSegmentThresholdBytes = 100LL * 1024 * 1024;
} // namespace

MergeOnFlushMergePolicy::MergeOnFlushMergePolicy(std::unique_ptr<MergePolicy> delegate)
    : delegate_(std::move(delegate)), small_segment_threshold_bytes_(kDefaultSmallSegmentThresholdBytes) {
    if (delegate_ == nullptr) {
        delegate_ = std::make_unique<NoMergePolicy>();
    }
}

double MergeOnFlushMergePolicy::small_segment_threshold_mb() const noexcept {
    return bytes_to_mb(small_segment_threshold_bytes_);
}

void MergeOnFlushMergePolicy::set_small_segment_threshold_mb(double megabytes) noexcept {
    small_segment_threshold_bytes_ = mb_to_bytes(megabytes);
}

std::string MergeOnFlushMergePolicy::to_string() const {
    return "MergeOnFlushMergePolicy(" + delegate_->to_string() + ")";
}

Result<std::optional<MergeSpecification>> MergeOnFlushMergePolicy::small_segment_merge(
    const SegmentInfos &infos, const MergeContext &context) const {
    const std::set<std::string> merging = context.get_merging_segments();
    std::vector<SegmentCommitInfo> small;
    small.reserve(infos.size());
    for (const SegmentCommitInfo &info : infos.segments) {
        if (merging.contains(info.info.name)) {
            continue;
        }
        auto bytes = segment_size_in_bytes(info, context);
        if (!bytes.has_value()) {
            return std::unexpected(std::move(bytes.error()));
        }
        if (bytes.value() < small_segment_threshold_bytes_) {
            small.push_back(info);
        }
    }
    if (small.size() < 2) {
        return std::optional<MergeSpecification>{};
    }
    MergeSpecification spec;
    spec.add(OneMerge(std::move(small)));
    return std::optional<MergeSpecification>{std::move(spec)};
}

Result<std::optional<MergeSpecification>> MergeOnFlushMergePolicy::find_merges(MergeTrigger merge_trigger,
                                                                              const SegmentInfos &infos,
                                                                              const MergeContext &context) const {
    if (merge_trigger == MergeTrigger::FullFlush) {
        return small_segment_merge(infos, context);
    }
    return delegate_->find_merges(merge_trigger, infos, context);
}

Result<std::optional<MergeSpecification>> MergeOnFlushMergePolicy::find_forced_merges(
    const SegmentInfos &infos, int32_t max_num_segments, const MergeContext &context) const {
    return delegate_->find_forced_merges(infos, max_num_segments, context);
}

Result<std::optional<MergeSpecification>> MergeOnFlushMergePolicy::find_forced_deletes_merges(
    const SegmentInfos &infos, const MergeContext &context) const {
    return delegate_->find_forced_deletes_merges(infos, context);
}

bool MergeOnFlushMergePolicy::use_compound_file(const SegmentInfos &infos, const SegmentCommitInfo &merged_info,
                                               const MergeContext &context) const {
    return delegate_->use_compound_file(infos, merged_info, context);
}

int64_t MergeOnFlushMergePolicy::max_full_flush_merge_size() const {
    return delegate_->max_full_flush_merge_size();
}

} // namespace lucene
