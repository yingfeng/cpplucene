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

module lucene_core:log_merge_policy.impl;

import :log_merge_policy;

import std;

import :error;
import :merge_policy;
import :merge_trigger;
import :segment_commit_info;
import :segment_infos;

namespace lucene {

namespace {

constexpr double MB = 1024.0 * 1024.0;

/// The merge of the segments `[start, end)` of `infos`.
[[nodiscard]] OneMerge make_merge(const SegmentInfos &infos, int32_t start, int32_t end) {
    const auto first = infos.segments.begin() + start;
    const auto last = infos.segments.begin() + end;
    return OneMerge(std::vector<SegmentCommitInfo>(first, last));
}

} // namespace

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------

VoidResult LogMergePolicy::set_merge_factor(int32_t merge_factor) {
    if (merge_factor < 2) {
        return err(LuceneError::illegal_argument("mergeFactor cannot be less than 2"));
    }
    merge_factor_ = merge_factor;
    return {};
}

double LogMergePolicy::min_merge_mb() const noexcept { return static_cast<double>(min_merge_size_) / MB; }

VoidResult LogMergePolicy::set_min_merge_mb(double min_merge_mb) {
    if (min_merge_mb < 0.0) {
        return err(LuceneError::illegal_argument(std::format("minMergeMB must be >= 0 (got {})", min_merge_mb)));
    }
    min_merge_size_ = static_cast<int64_t>(min_merge_mb * MB);
    return {};
}

double LogMergePolicy::max_merge_mb() const noexcept { return static_cast<double>(max_merge_size_) / MB; }

VoidResult LogMergePolicy::set_max_merge_mb(double max_merge_mb) {
    if (max_merge_mb < 0.0) {
        return err(LuceneError::illegal_argument(std::format("maxMergeMB must be >= 0 (got {})", max_merge_mb)));
    }
    max_merge_size_ = static_cast<int64_t>(max_merge_mb * MB);
    return {};
}

double LogMergePolicy::max_merge_mb_for_forced_merge() const noexcept {
    return static_cast<double>(max_merge_size_for_forced_merge_) / MB;
}

VoidResult LogMergePolicy::set_max_merge_mb_for_forced_merge(double max_merge_mb) {
    if (max_merge_mb < 0.0) {
        return err(LuceneError::illegal_argument(
                std::format("maxMergeMBForForcedMerge must be >= 0 (got {})", max_merge_mb)));
    }
    max_merge_size_for_forced_merge_ = static_cast<int64_t>(max_merge_mb * MB);
    return {};
}

VoidResult LogMergePolicy::set_target_search_concurrency(int32_t target_search_concurrency) {
    if (target_search_concurrency < 1) {
        return err(LuceneError::illegal_argument(
                std::format("targetSearchConcurrency must be >= 1 (got {})", target_search_concurrency)));
    }
    target_search_concurrency_ = target_search_concurrency;
    return {};
}

// ---------------------------------------------------------------------------
// size helpers
// ---------------------------------------------------------------------------

Result<int64_t> LogMergePolicy::size_docs(const SegmentCommitInfo &info, const MergeContext &context) const {
    if (!calibrate_size_by_deletes_) {
        return static_cast<int64_t>(info.max_doc());
    }
    auto deleted = context.num_deletes_to_merge(info);
    if (!deleted.has_value()) {
        return std::unexpected(std::move(deleted.error()));
    }
    debug_check(deleted.value() >= 0 && deleted.value() <= info.max_doc(), "delete count out of range");
    return static_cast<int64_t>(info.max_doc()) - deleted.value();
}

Result<int64_t> LogMergePolicy::size_bytes(const SegmentCommitInfo &info, const MergeContext &context) const {
    auto bytes = segment_size_in_bytes(info, context);
    if (!bytes.has_value()) {
        return std::unexpected(std::move(bytes.error()));
    }
    if (!calibrate_size_by_deletes_) {
        return bytes.value();
    }
    auto deleted = context.num_deletes_to_merge(info);
    if (!deleted.has_value()) {
        return std::unexpected(std::move(deleted.error()));
    }
    const int64_t max_doc = info.max_doc();
    if (max_doc <= 0) {
        return bytes.value();
    }
    const double del_ratio = static_cast<double>(deleted.value()) / static_cast<double>(max_doc);
    debug_check(del_ratio <= 1.0, "delete ratio out of range");
    return static_cast<int64_t>(static_cast<double>(bytes.value()) * (1.0 - del_ratio));
}

Result<bool> LogMergePolicy::is_merged(const SegmentInfos &infos, int32_t max_num_segments,
                                       const MergeContext &context) const {
    const int32_t num_segments = static_cast<int32_t>(infos.size());
    if (num_segments > max_num_segments) {
        return false;
    }
    // Every segment is eligible for the merge, so with a single segment the
    // index is merged only when that segment also has no pending deletes.
    if (num_segments == 1) {
        return has_merged(infos, infos.segments[0], context);
    }
    return true;
}

// ---------------------------------------------------------------------------
// find_merges
// ---------------------------------------------------------------------------

Result<std::optional<MergeSpecification>> LogMergePolicy::find_merges(MergeTrigger /*merge_trigger*/,
                                                                     const SegmentInfos &infos,
                                                                     const MergeContext &context) const {
    const int32_t num_segments = static_cast<int32_t>(infos.size());
    if (num_segments == 0) {
        return std::optional<MergeSpecification>{};
    }

    // A level is log (base mergeFactor) of the size of a segment.
    const double norm = std::log(static_cast<double>(merge_factor_));
    const std::set<std::string> merging_segments = context.get_merging_segments();

    int64_t total_doc_count = 0;
    std::vector<float> levels;
    levels.reserve(static_cast<size_t>(num_segments));
    for (int32_t i = 0; i < num_segments; ++i) {
        const SegmentCommitInfo &info = infos.segments[i];
        auto docs = size_docs(info, context);
        if (!docs.has_value()) {
            return std::unexpected(std::move(docs.error()));
        }
        total_doc_count += docs.value();
        auto size = this->size(info, context);
        if (!size.has_value()) {
            return std::unexpected(std::move(size.error()));
        }
        // Floor tiny segments.
        const int64_t floored = std::max<int64_t>(size.value(), 1);
        levels.push_back(static_cast<float>(std::log(static_cast<double>(floored)) / norm));
    }

    const float level_floor = min_merge_size_ <= 0
                                      ? 0.0F
                                      : static_cast<float>(std::log(static_cast<double>(min_merge_size_)) / norm);

    // The level of a segment is any size within LEVEL_LOG_SPAN of the biggest
    // remaining segment, or one that has such a segment to its right.
    const int32_t num_mergeable_segments = static_cast<int32_t>(levels.size());
    std::vector<float> max_levels(static_cast<size_t>(num_mergeable_segments) + 1, 0.0F);
    // -1 is the minimum possible level, because ln(1) is 0.
    max_levels[static_cast<size_t>(num_mergeable_segments)] = -1.0F;
    for (int32_t i = num_mergeable_segments - 1; i >= 0; --i) {
        const size_t index = static_cast<size_t>(i);
        max_levels[index] = std::max(levels[index], max_levels[index + 1]);
    }

    std::optional<MergeSpecification> spec;

    // Documents a merge may cover: the policy limit, but at most the share of a
    // search slice.
    const int64_t tsc = std::max<int32_t>(target_search_concurrency_, 1);
    const int64_t max_merge_docs = std::min<int64_t>(static_cast<int64_t>(max_merge_docs_),
                                                     (total_doc_count + tsc - 1) / tsc);

    int32_t start = 0;
    while (start < num_mergeable_segments) {
        const float max_level = max_levels[static_cast<size_t>(start)];

        // Segments below the floor size may be unbalanced by twice as much.
        const float level_bottom =
                max_level > level_floor ? max_level - static_cast<float>(LEVEL_LOG_SPAN)
                                        : max_level - static_cast<float>(2.0 * LEVEL_LOG_SPAN);

        int32_t upto = num_mergeable_segments - 1;
        while (upto >= start && levels[static_cast<size_t>(upto)] < level_bottom) {
            --upto;
        }

        int32_t end = start + merge_factor_;
        while (end <= 1 + upto) {
            bool any_merging = false;
            int64_t merge_size = 0;
            int64_t merge_docs = 0;

            int32_t i = start;
            while (i < end) {
                const SegmentCommitInfo &info = infos.segments[static_cast<size_t>(i)];
                if (merging_segments.contains(info.info.name)) {
                    any_merging = true;
                    break;
                }

                auto segment_size = this->size(info, context);
                if (!segment_size.has_value()) {
                    return std::unexpected(std::move(segment_size.error()));
                }
                auto segment_docs = size_docs(info, context);
                if (!segment_docs.has_value()) {
                    return std::unexpected(std::move(segment_docs.error()));
                }

                if (merge_size + segment_size.value() > max_merge_size_ ||
                    merge_docs + segment_docs.value() > max_merge_docs) {
                    // This merge is full: keep the segments collected so far,
                    // unless the first one alone is already too large, in which
                    // case it is returned as a singleton merge.
                    end = i == start ? i + 1 : i;
                    break;
                }

                merge_size += segment_size.value();
                merge_docs += segment_docs.value();
                ++i;
            }

            if (!any_merging && end - start > 1) {
                if (!spec.has_value()) {
                    spec.emplace();
                }
                spec->add(make_merge(infos, start, end));
            }

            start = end;
            end = start + merge_factor_;
        }

        start = 1 + upto;
    }

    return spec;
}

// ---------------------------------------------------------------------------
// forced merges
// ---------------------------------------------------------------------------

Result<std::optional<MergeSpecification>> LogMergePolicy::find_forced_merges(const SegmentInfos &infos,
                                                                            int32_t max_num_segments,
                                                                            const MergeContext &context) const {
    if (max_num_segments <= 0) {
        return err(LuceneError::illegal_argument("maxNumSegments must be > 0"));
    }
    const int32_t num_segments = static_cast<int32_t>(infos.size());
    if (num_segments == 0) {
        return std::optional<MergeSpecification>{};
    }

    auto merged = is_merged(infos, max_num_segments, context);
    if (!merged.has_value()) {
        return std::unexpected(std::move(merged.error()));
    }
    if (merged.value()) {
        return std::optional<MergeSpecification>{};
    }

    // Every segment is eligible, so the rightmost segment that has to be merged
    // is the last one.
    const int32_t last = num_segments;
    if (max_num_segments == 1 && last == 1) {
        auto single = has_merged(infos, infos.segments[0], context);
        if (!single.has_value()) {
            return std::unexpected(std::move(single.error()));
        }
        if (single.value()) {
            return std::optional<MergeSpecification>{};
        }
    }

    // A segment above a hard limit makes the plain "merge down to N" plan
    // unusable, because such a segment can never take part in a merge.
    bool any_too_large = false;
    for (int32_t i = 0; i < last; ++i) {
        const SegmentCommitInfo &info = infos.segments[static_cast<size_t>(i)];
        auto size = this->size(info, context);
        if (!size.has_value()) {
            return std::unexpected(std::move(size.error()));
        }
        auto docs = size_docs(info, context);
        if (!docs.has_value()) {
            return std::unexpected(std::move(docs.error()));
        }
        if (size.value() > max_merge_size_for_forced_merge_ || docs.value() > max_merge_docs_) {
            any_too_large = true;
            break;
        }
    }

    if (any_too_large) {
        return find_forced_merges_size_limit(infos, last, context);
    }
    return find_forced_merges_max_num_segments(infos, max_num_segments, last, context);
}

Result<std::optional<MergeSpecification>> LogMergePolicy::find_forced_merges_size_limit(
        const SegmentInfos &infos, int32_t last, const MergeContext &context) const {
    debug_check(last > 0, "last must be positive");
    MergeSpecification spec;
    int32_t start = last - 1;
    int32_t end = last;

    while (start >= 0) {
        const SegmentCommitInfo &info = infos.segments[static_cast<size_t>(start)];
        auto size = this->size(info, context);
        if (!size.has_value()) {
            return std::unexpected(std::move(size.error()));
        }
        auto docs = size_docs(info, context);
        if (!docs.has_value()) {
            return std::unexpected(std::move(docs.error()));
        }

        if (size.value() > max_merge_size_for_forced_merge_ || docs.value() > max_merge_docs_) {
            // Skip that segment and merge the segments to its right, unless
            // there is only one and it is already merged.
            bool add = end - start - 1 > 1;
            if (!add && start != end - 1) {
                auto right_merged = has_merged(infos, infos.segments[static_cast<size_t>(start + 1)], context);
                if (!right_merged.has_value()) {
                    return std::unexpected(std::move(right_merged.error()));
                }
                add = !right_merged.value();
            }
            if (add && start + 1 < end) {
                spec.add(make_merge(infos, start + 1, end));
            }
            end = start;
        } else if (end - start == merge_factor_) {
            // mergeFactor eligible segments were found.
            spec.add(make_merge(infos, start, end));
            end = start;
        }
        --start;
    }

    // Add the left-over segments, unless there is just one already fully merged.
    ++start;
    if (end > 0 && start < end) {
        bool add = start + 1 < end;
        if (!add) {
            auto left_merged = has_merged(infos, infos.segments[static_cast<size_t>(start)], context);
            if (!left_merged.has_value()) {
                return std::unexpected(std::move(left_merged.error()));
            }
            add = !left_merged.value();
        }
        if (add) {
            spec.add(make_merge(infos, start, end));
        }
    }

    if (spec.empty()) {
        return std::optional<MergeSpecification>{};
    }
    return std::optional<MergeSpecification>(std::move(spec));
}

Result<std::optional<MergeSpecification>> LogMergePolicy::find_forced_merges_max_num_segments(
        const SegmentInfos &infos, int32_t max_num_segments, int32_t last, const MergeContext &context) const {
    MergeSpecification spec;

    // Enroll all "full" merges (size mergeFactor) so they may run concurrently.
    while (last + 1 >= merge_factor_ + max_num_segments) {
        const int32_t start = last - merge_factor_;
        spec.add(make_merge(infos, start, last));
        last -= merge_factor_;
    }

    // Only if no full merge is pending, add the final partial merge.
    if (spec.empty()) {
        if (max_num_segments == 1) {
            bool add = last > 1;
            if (!add) {
                auto single = has_merged(infos, infos.segments[0], context);
                if (!single.has_value()) {
                    return std::unexpected(std::move(single.error()));
                }
                add = !single.value();
            }
            if (add) {
                spec.add(make_merge(infos, 0, last));
            }
        } else if (last > max_num_segments) {
            // Pick the cheapest partial merge that does not make the index too
            // lopsided.
            const int32_t final_merge_size = last - max_num_segments + 1;
            int64_t best_size = 0;
            int32_t best_start = 0;

            const int32_t limit = last - final_merge_size + 1;
            for (int32_t i = 0; i < limit; ++i) {
                int64_t sum_size = 0;
                for (int32_t j = 0; j < final_merge_size; ++j) {
                    auto size = this->size(infos.segments[static_cast<size_t>(j + i)], context);
                    if (!size.has_value()) {
                        return std::unexpected(std::move(size.error()));
                    }
                    sum_size += size.value();
                }

                bool take = i == 0;
                if (!take) {
                    auto previous = this->size(infos.segments[static_cast<size_t>(i - 1)], context);
                    if (!previous.has_value()) {
                        return std::unexpected(std::move(previous.error()));
                    }
                    take = sum_size < 2 * previous.value() && sum_size < best_size;
                }
                if (take) {
                    best_start = i;
                    best_size = sum_size;
                }
            }

            spec.add(make_merge(infos, best_start, best_start + final_merge_size));
        }
    }

    if (spec.empty()) {
        return std::optional<MergeSpecification>{};
    }
    return std::optional<MergeSpecification>(std::move(spec));
}

// ---------------------------------------------------------------------------
// forced deletes merges
// ---------------------------------------------------------------------------

Result<std::optional<MergeSpecification>> LogMergePolicy::find_forced_deletes_merges(const SegmentInfos &infos,
                                                                                     const MergeContext &context) const {
    const int32_t num_segments = static_cast<int32_t>(infos.size());
    MergeSpecification spec;
    std::optional<int32_t> first_segment_with_deletions;

    for (int32_t i = 0; i < num_segments; ++i) {
        const SegmentCommitInfo &info = infos.segments[static_cast<size_t>(i)];
        auto deleted = context.num_deletes_to_merge(info);
        if (!deleted.has_value()) {
            return std::unexpected(std::move(deleted.error()));
        }
        debug_check(deleted.value() >= 0 && deleted.value() <= info.max_doc(), "delete count out of range");

        if (deleted.value() > 0) {
            if (!first_segment_with_deletions.has_value()) {
                first_segment_with_deletions = i;
            } else if (i == first_segment_with_deletions.value() + merge_factor_) {
                // mergeFactor segments in a row have deletions: merge now.
                spec.add(make_merge(infos, first_segment_with_deletions.value(), i));
                first_segment_with_deletions = i;
            }
        } else if (first_segment_with_deletions.has_value()) {
            // End of a run of segments with deletions: merge them even when
            // there are fewer than mergeFactor of them.
            spec.add(make_merge(infos, first_segment_with_deletions.value(), i));
            first_segment_with_deletions.reset();
        }
    }

    if (first_segment_with_deletions.has_value()) {
        spec.add(make_merge(infos, first_segment_with_deletions.value(), num_segments));
    }

    return std::optional<MergeSpecification>(std::move(spec));
}

// ---------------------------------------------------------------------------
// LogDocMergePolicy
// ---------------------------------------------------------------------------

std::unique_ptr<LogDocMergePolicy> LogDocMergePolicy::create() {
    std::unique_ptr<LogDocMergePolicy> policy(new LogDocMergePolicy());
    policy->min_merge_size_ = DEFAULT_MIN_MERGE_DOCS;
    // The size limits are never used by this policy, so they are disabled.
    policy->max_merge_size_ = std::numeric_limits<int64_t>::max();
    policy->max_merge_size_for_forced_merge_ = std::numeric_limits<int64_t>::max();
    return policy;
}

std::string LogDocMergePolicy::to_string() const {
    return std::format("[LogDocMergePolicy: minMergeDocs={}, mergeFactor={}, maxMergeDocs={}, calibrateSizeByDeletes={}, "
                       "noCFSRatio={}]",
                       min_merge_size_, merge_factor_, max_merge_docs_, calibrate_size_by_deletes_, no_cfs_ratio());
}

Result<int64_t> LogDocMergePolicy::size(const SegmentCommitInfo &info, const MergeContext &context) const {
    return size_docs(info, context);
}

// ---------------------------------------------------------------------------
// LogByteSizeMergePolicy
// ---------------------------------------------------------------------------

std::unique_ptr<LogByteSizeMergePolicy> LogByteSizeMergePolicy::create() {
    std::unique_ptr<LogByteSizeMergePolicy> policy(new LogByteSizeMergePolicy());
    policy->min_merge_size_ = static_cast<int64_t>(DEFAULT_MIN_MERGE_MB * MB);
    policy->max_merge_size_ = static_cast<int64_t>(DEFAULT_MAX_MERGE_MB * MB);
    policy->max_merge_size_for_forced_merge_ = std::numeric_limits<int64_t>::max();
    return policy;
}

std::string LogByteSizeMergePolicy::to_string() const {
    return std::format("[LogByteSizeMergePolicy: minMergeMB={}, mergeFactor={}, maxMergeMB={}, "
                       "maxMergeMBForForcedMerge={}, calibrateSizeByDeletes={}, maxMergeDocs={}, noCFSRatio={}]",
                       min_merge_mb(), merge_factor_, max_merge_mb(), max_merge_mb_for_forced_merge(),
                       calibrate_size_by_deletes_, max_merge_docs_, no_cfs_ratio());
}

Result<int64_t> LogByteSizeMergePolicy::size(const SegmentCommitInfo &info, const MergeContext &context) const {
    return size_bytes(info, context);
}

} // namespace lucene
