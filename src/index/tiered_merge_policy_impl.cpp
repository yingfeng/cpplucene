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

module lucene_core:tiered_merge_policy.impl;

import :tiered_merge_policy;

import std;

import :error;
import :merge_policy;
import :merge_trigger;
import :segment_commit_info;
import :segment_infos;

namespace lucene {

namespace {

constexpr double MB = 1024.0 * 1024.0;

/// The merge of the segments described by `segments`.
[[nodiscard]] OneMerge merge_of(std::span<const SegmentCommitInfo *const> segments) {
    std::vector<SegmentCommitInfo> copies;
    copies.reserve(segments.size());
    for (const SegmentCommitInfo *segment : segments) {
        copies.push_back(*segment);
    }
    return OneMerge(std::move(copies));
}

} // namespace

std::string TieredMergePolicy::MergeScore::explanation() const {
    return std::format("skew={:.3} nonDelRatio={:.3}", skew, non_del_ratio);
}

TieredMergePolicy::TieredMergePolicy()
        : max_merge_at_once_(DEFAULT_MAX_MERGE_AT_ONCE),
          max_merged_segment_bytes_(DEFAULT_MAX_MERGED_SEGMENT_BYTES),
          floor_segment_bytes_(DEFAULT_FLOOR_SEGMENT_BYTES),
          segments_per_tier_(DEFAULT_SEGS_PER_TIER),
          force_merge_deletes_pct_allowed_(DEFAULT_FORCE_MERGE_DELETES_PCT_ALLOWED),
          deletes_pct_allowed_(DEFAULT_DELETES_PCT_ALLOWED),
          target_search_concurrency_(1) {}

std::unique_ptr<TieredMergePolicy> TieredMergePolicy::create() {
    return std::unique_ptr<TieredMergePolicy>(new TieredMergePolicy());
}

std::string TieredMergePolicy::to_string() const {
    return std::format("[TieredMergePolicy: maxMergeAtOnce={}, maxMergedSegmentMB={}, floorSegmentMB={}, "
                       "forceMergeDeletesPctAllowed={}, segmentsPerTier={}, maxCFSSegmentSizeMB={}, "
                       "noCFSRatio={}, deletesPctAllowed={}, targetSearchConcurrency={}]",
                       max_merge_at_once_, max_merged_segment_mb(), floor_segment_mb(),
                       force_merge_deletes_pct_allowed_, segments_per_tier_, max_cfs_segment_size_mb(),
                       no_cfs_ratio(), deletes_pct_allowed_, target_search_concurrency_);
}

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------

VoidResult TieredMergePolicy::set_max_merge_at_once(int32_t value) {
    if (value < 2) {
        return err(LuceneError::illegal_argument(std::format("maxMergeAtOnce must be > 1 (got {})", value)));
    }
    max_merge_at_once_ = value;
    return {};
}

double TieredMergePolicy::max_merged_segment_mb() const noexcept {
    return static_cast<double>(max_merged_segment_bytes_) / MB;
}

VoidResult TieredMergePolicy::set_max_merged_segment_mb(double value) {
    if (value < 0.0) {
        return err(LuceneError::illegal_argument(std::format("maxMergedSegmentMB must be >=0 (got {})", value)));
    }
    const double bytes = value * MB;
    max_merged_segment_bytes_ = bytes >= static_cast<double>(std::numeric_limits<int64_t>::max())
                                        ? std::numeric_limits<int64_t>::max()
                                        : static_cast<int64_t>(bytes);
    return {};
}

double TieredMergePolicy::floor_segment_mb() const noexcept {
    return static_cast<double>(floor_segment_bytes_) / MB;
}

VoidResult TieredMergePolicy::set_floor_segment_mb(double value) {
    if (value < 0.0) {
        return err(LuceneError::illegal_argument(std::format("floorSegmentMB must be >= 0 (got {})", value)));
    }
    const double bytes = value * MB;
    floor_segment_bytes_ = bytes >= static_cast<double>(std::numeric_limits<int64_t>::max())
                                   ? std::numeric_limits<int64_t>::max()
                                   : static_cast<int64_t>(bytes);
    return {};
}

VoidResult TieredMergePolicy::set_segments_per_tier(double value) {
    if (value < 2.0) {
        return err(LuceneError::illegal_argument(std::format("segmentsPerTier must be >= 2.0 (got {})", value)));
    }
    segments_per_tier_ = value;
    return {};
}

VoidResult TieredMergePolicy::set_deletes_pct_allowed(double value) {
    if (value < 0.0 || value > 100.0) {
        return err(LuceneError::illegal_argument(
                std::format("deletesPctAllowed must be between 0% and 100% inclusive; got {}", value)));
    }
    deletes_pct_allowed_ = value;
    return {};
}

VoidResult TieredMergePolicy::set_force_merge_deletes_pct_allowed(double value) {
    if (value < 0.0) {
        return err(LuceneError::illegal_argument(
                std::format("forceMergeDeletesPctAllowed must be >= 0 (got {})", value)));
    }
    force_merge_deletes_pct_allowed_ = value;
    return {};
}

VoidResult TieredMergePolicy::set_target_search_concurrency(int32_t value) {
    if (value < 1) {
        return err(LuceneError::illegal_argument(
                std::format("targetSearchConcurrency must be >= 1 (got {})", value)));
    }
    target_search_concurrency_ = value;
    return {};
}

// ---------------------------------------------------------------------------
// size helpers
// ---------------------------------------------------------------------------

int32_t TieredMergePolicy::get_max_allowed_docs(int32_t total_max_doc, int32_t total_del_docs) const {
    const int32_t live = total_max_doc - total_del_docs;
    return live / target_search_concurrency_ + (live % target_search_concurrency_ > 0 ? 1 : 0);
}

int64_t TieredMergePolicy::floor_size(int64_t bytes) const {
    return std::max(floor_segment_bytes_, bytes);
}

Result<std::vector<TieredMergePolicy::SegmentSizeAndDocs>> TieredMergePolicy::get_sorted_by_segment_size(
        const SegmentInfos &infos, const MergeContext &context) const {
    std::vector<SegmentSizeAndDocs> sorted;
    sorted.reserve(infos.size());

    for (const SegmentCommitInfo &info : infos.segments) {
        auto bytes = calibrated_size_in_bytes(info, context);
        if (!bytes.has_value()) {
            return std::unexpected(std::move(bytes.error()));
        }
        auto raw = segment_size_in_bytes(info, context);
        if (!raw.has_value()) {
            return std::unexpected(std::move(raw.error()));
        }
        auto deleted = context.num_deletes_to_merge(info);
        if (!deleted.has_value()) {
            return std::unexpected(std::move(deleted.error()));
        }
        debug_check(deleted.value() >= 0 && deleted.value() <= info.max_doc(), "delete count out of range");
        sorted.push_back(SegmentSizeAndDocs{&info, bytes.value(), raw.value(), deleted.value(), info.max_doc()});
    }

    std::ranges::sort(sorted, [](const SegmentSizeAndDocs &left, const SegmentSizeAndDocs &right) {
        if (left.size_in_bytes != right.size_in_bytes) {
            return left.size_in_bytes > right.size_in_bytes;
        }
        return left.seg_info->info.name < right.seg_info->info.name;
    });
    return sorted;
}

// ---------------------------------------------------------------------------
// scoring
// ---------------------------------------------------------------------------

Result<TieredMergePolicy::MergeScore> TieredMergePolicy::score(
        std::span<const SegmentSizeAndDocs> candidate, bool hit_too_large,
        const std::map<std::string, SegmentSizeAndDocs> &segments_sizes) const {
    int64_t tot_before_merge_bytes = 0;
    int64_t tot_after_merge_bytes = 0;
    int64_t tot_after_merge_bytes_floored = 0;

    for (const SegmentSizeAndDocs &info : candidate) {
        auto found = segments_sizes.find(info.seg_info->info.name);
        if (found == segments_sizes.end()) {
            return err(LuceneError::illegal_state("candidate segment size is missing"));
        }
        tot_after_merge_bytes += found->second.size_in_bytes;
        tot_after_merge_bytes_floored += floor_size(found->second.size_in_bytes);
        tot_before_merge_bytes += info.size_in_seg;
    }

    // How "balanced" the merge is: 1 / numSegs (good) to 1.0 (poor). Lopsided
    // merges mean O(N^2) merge cost over time.
    double skew = 0.0;
    if (hit_too_large) {
        // Pretend perfect skew: this merge does not cascade, so it cannot cause
        // N^2 cost.
        const int32_t merge_factor = std::min(max_merge_at_once_, static_cast<int32_t>(segments_per_tier_));
        skew = 1.0 / static_cast<double>(merge_factor);
    } else {
        auto first = segments_sizes.find(candidate[0].seg_info->info.name);
        if (first == segments_sizes.end()) {
            return err(LuceneError::illegal_state("candidate segment size is missing"));
        }
        skew = static_cast<double>(floor_size(first->second.size_in_bytes)) /
               static_cast<double>(tot_after_merge_bytes_floored);
    }

    // Less skew is strongly preferred.
    double merge_score = skew;
    // Gently prefer smaller merges: not too large an exponent, or the policy
    // would merge small segments just to avoid the big merges.
    merge_score *= std::pow(static_cast<double>(tot_after_merge_bytes), 0.05);
    // Strongly prefer merges that reclaim deletes.
    const double non_del_ratio = static_cast<double>(tot_after_merge_bytes) /
                                 static_cast<double>(std::max<int64_t>(tot_before_merge_bytes, 1));
    merge_score *= std::pow(non_del_ratio, 2.0);

    return MergeScore{merge_score, skew, non_del_ratio};
}

// ---------------------------------------------------------------------------
// do_find_merges
// ---------------------------------------------------------------------------

Result<std::optional<MergeSpecification>> TieredMergePolicy::do_find_merges(
        std::vector<SegmentSizeAndDocs> sorted_eligible, int64_t max_merged_segment_bytes, int32_t merge_factor,
        size_t allowed_seg_count, int32_t allowed_del_count, int32_t allowed_doc_count, MergeType merge_type,
        const MergeContext &context, bool max_merge_is_running) const {
    std::map<std::string, SegmentSizeAndDocs> segments_sizes;
    for (const SegmentSizeAndDocs &segment : sorted_eligible) {
        segments_sizes.emplace(segment.seg_info->info.name, segment);
    }

    const size_t original_count = sorted_eligible.size();
    message(std::format("findMerges: {} segments", original_count), context);
    if (original_count == 0) {
        return std::optional<MergeSpecification>{};
    }

    std::set<std::string> to_be_merged;
    std::optional<MergeSpecification> spec;
    // The trigger point for the total number of deletes leads to a bunch of
    // large merges at the same time, so only one of them is queued per call.
    bool have_one_large_merge = false;

    while (true) {
        // Only segments that are neither being merged nor already picked by a
        // previous iteration are eligible.
        std::erase_if(sorted_eligible,
                      [&to_be_merged](const SegmentSizeAndDocs &segment) {
                          return to_be_merged.contains(segment.seg_info->info.name);
                      });

        message(std::format("  allowedSegmentCount={} vs count={} (eligible count={})", allowed_seg_count,
                            original_count, sorted_eligible.size()),
                context);

        if (sorted_eligible.empty()) {
            return spec;
        }

        int32_t remaining_del_count = 0;
        for (const SegmentSizeAndDocs &segment : sorted_eligible) {
            remaining_del_count += segment.del_count;
        }
        if (merge_type == MergeType::Natural && sorted_eligible.size() <= allowed_seg_count &&
            remaining_del_count <= allowed_del_count) {
            return spec;
        }

        // Over budget: find the best merge.
        std::optional<MergeScore> best_score;
        std::vector<SegmentSizeAndDocs> best;
        bool best_too_large = false;
        int64_t best_merge_bytes = 0;

        std::vector<SegmentSizeAndDocs> candidate;
        for (size_t start_idx = 0; start_idx < sorted_eligible.size(); ++start_idx) {
            candidate.clear();
            bool hit_too_large = false;
            int64_t bytes_this_merge = 0;
            int64_t doc_count_this_merge = 0;

            size_t idx = start_idx;
            while (idx < sorted_eligible.size() &&
                   candidate.size() < static_cast<size_t>(max_merge_at_once_) &&
                   // More than mergeFactor segments may be merged when the result
                   // stays below the floor size, so small segments grow quickly.
                   (candidate.size() < static_cast<size_t>(merge_factor) || bytes_this_merge < floor_segment_bytes_) &&
                   bytes_this_merge < max_merged_segment_bytes &&
                   (bytes_this_merge < floor_segment_bytes_ || doc_count_this_merge <= allowed_doc_count)) {
                const SegmentSizeAndDocs &segment = sorted_eligible[idx];
                const int64_t segment_bytes = segment.size_in_bytes;
                const int64_t segment_docs = static_cast<int64_t>(segment.max_doc) - segment.del_count;

                if (bytes_this_merge + segment_bytes > max_merged_segment_bytes ||
                    (bytes_this_merge > floor_segment_bytes_ && doc_count_this_merge + segment_docs > allowed_doc_count)) {
                    // `hit_too_large` only tracks the byte limit: such segments are
                    // as large as they may get and stay ineligible for a long time.
                    hit_too_large = hit_too_large || bytes_this_merge + segment_bytes > max_merged_segment_bytes;
                    if (!candidate.empty()) {
                        // Keep going so smaller segments can be packed in.
                        ++idx;
                        continue;
                    }
                }

                candidate.push_back(segment);
                bytes_this_merge += segment_bytes;
                doc_count_this_merge += segment_docs;
                ++idx;
            }

            if (candidate.empty()) {
                continue;
            }

            auto largest = segments_sizes.find(candidate[0].seg_info->info.name);
            if (largest == segments_sizes.end()) {
                return err(LuceneError::illegal_state("could not find candidate"));
            }
            const SegmentSizeAndDocs &largest_candidate = largest->second;

            if (!hit_too_large && merge_type == MergeType::Natural &&
                static_cast<double>(bytes_this_merge) < static_cast<double>(largest_candidate.size_in_bytes) * 1.5 &&
                static_cast<double>(largest_candidate.del_count) <
                        static_cast<double>(largest_candidate.max_doc) * deletes_pct_allowed_ / 100.0) {
                // Ignore merges whose result is not at least 50% larger than their
                // biggest input, unless they reclaim a lot of deletes: otherwise
                // the same big segment would be rewritten over and over.
                continue;
            }

            // A singleton merge without deletes makes no sense.
            if (candidate.size() == 1 && largest_candidate.del_count == 0) {
                continue;
            }

            // Past the tail of the list only smaller merges remain.
            if (best_score.has_value() && !hit_too_large && candidate.size() < static_cast<size_t>(merge_factor)) {
                break;
            }

            auto candidate_score = score(candidate, hit_too_large, segments_sizes);
            if (!candidate_score.has_value()) {
                return std::unexpected(std::move(candidate_score.error()));
            }

            if ((!best_score.has_value() || candidate_score.value().score() < best_score.value().score()) &&
                (!hit_too_large || !max_merge_is_running)) {
                best = candidate;
                best_score = candidate_score.value();
                best_too_large = hit_too_large;
                best_merge_bytes = bytes_this_merge;
            }
        }

        if (best.empty()) {
            return spec;
        }

        if (!have_one_large_merge || !best_too_large || merge_type == MergeType::ForceMergeDeletes) {
            have_one_large_merge = have_one_large_merge || best_too_large;

            if (!spec.has_value()) {
                spec.emplace();
            }
            std::vector<const SegmentCommitInfo *> segments;
            segments.reserve(best.size());
            for (const SegmentSizeAndDocs &segment : best) {
                segments.push_back(segment.seg_info);
            }
            spec->add(merge_of(segments));
            message(std::format("  add merge={} size={:.3} MB score={:.3} {}{}", best.size(),
                                static_cast<double>(best_merge_bytes) / MB, best_score.value().score(),
                                best_score.value().explanation(), best_too_large ? " [max merge]" : ""),
                    context);
        }

        // Either way, these segments are out of consideration from now on.
        for (const SegmentSizeAndDocs &segment : best) {
            to_be_merged.insert(segment.seg_info->info.name);
        }
    }
}

// ---------------------------------------------------------------------------
// find_merges
// ---------------------------------------------------------------------------

Result<std::optional<MergeSpecification>> TieredMergePolicy::find_merges(MergeTrigger /*merge_trigger*/,
                                                                        const SegmentInfos &infos,
                                                                        const MergeContext &context) const {
    int64_t tot_index_bytes = 0;
    int64_t min_segment_bytes = std::numeric_limits<int64_t>::max();

    int32_t total_del_docs = 0;
    int32_t total_max_doc = 0;
    int64_t merging_bytes = 0;

    const std::set<std::string> merging = context.get_merging_segments();

    auto sorted_infos = get_sorted_by_segment_size(infos, context);
    if (!sorted_infos.has_value()) {
        return std::unexpected(std::move(sorted_infos.error()));
    }

    // Segments that are being merged are excluded, but their live documents still
    // count towards the index size.
    std::erase_if(sorted_infos.value(), [&](const SegmentSizeAndDocs &segment) {
        min_segment_bytes = std::min(segment.size_in_bytes, min_segment_bytes);
        tot_index_bytes += segment.size_in_bytes;
        if (merging.contains(segment.seg_info->info.name)) {
            merging_bytes += segment.size_in_bytes;
            total_max_doc += segment.max_doc - segment.del_count;
            return true;
        }
        total_del_docs += segment.del_count;
        total_max_doc += segment.max_doc;
        return false;
    });

    debug_check(total_max_doc >= 0 && total_del_docs >= 0, "document counts must not be negative");

    const double total_del_pct = total_max_doc == 0 ? 0.0
                                                    : 100.0 * static_cast<double>(total_del_docs) /
                                                              static_cast<double>(total_max_doc);
    int32_t allowed_del_count = static_cast<int32_t>(deletes_pct_allowed_ * static_cast<double>(total_max_doc) / 100.0);

    // Too-large segments and the segments kept for concurrency are graced out of
    // the maximum segment count.
    int32_t too_big_count = 0;
    int32_t concurrency_count = 0;
    double allowed_seg_count = 0.0;
    std::erase_if(sorted_infos.value(), [&](const SegmentSizeAndDocs &segment) {
        const double segment_del_pct = segment.max_doc == 0
                                               ? 0.0
                                               : 100.0 * static_cast<double>(segment.del_count) /
                                                         static_cast<double>(segment.max_doc);

        if (segment.size_in_bytes > max_merged_segment_bytes_ / 2 &&
            (total_del_pct <= deletes_pct_allowed_ || segment_del_pct <= deletes_pct_allowed_)) {
            ++too_big_count;
            tot_index_bytes -= segment.size_in_bytes;
            allowed_del_count -= segment.del_count;
            return true;
        }
        if (concurrency_count + too_big_count < target_search_concurrency_ - 1) {
            // Count a whole segment for the first targetSearchConcurrency-1
            // segments so the lower levels are not over-merged.
            ++concurrency_count;
            allowed_seg_count += 1.0;
            tot_index_bytes -= segment.size_in_bytes;
            return false;
        }
        return false;
    });

    allowed_del_count = std::max(0, allowed_del_count);

    const int32_t merge_factor = std::min(max_merge_at_once_, static_cast<int32_t>(segments_per_tier_));
    // Budget for the remaining segments, tier by tier.
    int64_t level_size = std::max(min_segment_bytes, floor_segment_bytes_);
    int64_t bytes_left = tot_index_bytes;

    while (true) {
        const double seg_count_level =
                static_cast<double>(bytes_left) / static_cast<double>(std::max<int64_t>(level_size, 1));
        if (seg_count_level < segments_per_tier_ || level_size == max_merged_segment_bytes_) {
            allowed_seg_count += std::ceil(seg_count_level);
            break;
        }
        allowed_seg_count += segments_per_tier_;
        bytes_left = static_cast<int64_t>(static_cast<double>(bytes_left) -
                                          segments_per_tier_ * static_cast<double>(level_size));
        level_size = std::min(max_merged_segment_bytes_, level_size * static_cast<int64_t>(merge_factor));
    }

    // Small segments may push the budget below one tier.
    allowed_seg_count = std::max(allowed_seg_count, segments_per_tier_);
    // No merge is needed when the index has no more segments than the target
    // search concurrency allows.
    allowed_seg_count = std::max(allowed_seg_count, static_cast<double>(target_search_concurrency_ - too_big_count));

    const int32_t allowed_doc_count = get_max_allowed_docs(total_max_doc, total_del_docs);

    return do_find_merges(std::move(sorted_infos.value()), max_merged_segment_bytes_, merge_factor,
                          static_cast<size_t>(allowed_seg_count), allowed_del_count, allowed_doc_count,
                          MergeType::Natural, context, merging_bytes >= max_merged_segment_bytes_);
}

// ---------------------------------------------------------------------------
// find_forced_merges
// ---------------------------------------------------------------------------

Result<std::optional<MergeSpecification>> TieredMergePolicy::find_forced_merges(const SegmentInfos &infos,
                                                                               int32_t max_num_segments,
                                                                               const MergeContext &context) const {
    if (max_num_segments <= 0) {
        return err(LuceneError::illegal_argument("maxNumSegments must be > 0"));
    }
    auto sorted_size_and_docs = get_sorted_by_segment_size(infos, context);
    if (!sorted_size_and_docs.has_value()) {
        return std::unexpected(std::move(sorted_size_and_docs.error()));
    }

    int64_t total_merge_bytes = 0;
    const std::set<std::string> merging = context.get_merging_segments();
    bool force_merge_running = false;

    // Every segment is eligible in this port, so only the segments that are
    // being merged right now are dropped.
    std::erase_if(sorted_size_and_docs.value(), [&](const SegmentSizeAndDocs &segment) {
        if (merging.contains(segment.seg_info->info.name)) {
            force_merge_running = true;
            return true;
        }
        total_merge_bytes += segment.size_in_bytes;
        return false;
    });

    int64_t max_merge_bytes = max_merged_segment_bytes_;
    if (max_num_segments == 1) {
        max_merge_bytes = std::numeric_limits<int64_t>::max();
    } else if (max_num_segments != std::numeric_limits<int32_t>::max()) {
        max_merge_bytes = std::max<int64_t>(
                static_cast<int64_t>(static_cast<double>(total_merge_bytes) / static_cast<double>(max_num_segments)),
                max_merged_segment_bytes_);
        // A 25% fudge, so a second pass is less likely to be needed to reach the
        // requested segment count.
        max_merge_bytes = static_cast<int64_t>(static_cast<double>(max_merge_bytes) * 1.25);
    }

    bool found_deletes = false;
    const bool unlimited_segments = max_num_segments == std::numeric_limits<int32_t>::max();

    std::erase_if(sorted_size_and_docs.value(), [&](const SegmentSizeAndDocs &segment) {
        if (segment.del_count != 0) {
            // This is a forced merge: every segment with deletes takes part.
            found_deletes = true;
            return false;
        }
        // Segments above the limit are never merged unless they have deletes.
        if (!unlimited_segments && segment.size_in_bytes >= max_merge_bytes) {
            return true;
        }
        return false;
    });

    if (sorted_size_and_docs.value().empty()) {
        return std::optional<MergeSpecification>{};
    }

    const size_t eligible_count = sorted_size_and_docs.value().size();
    if (!found_deletes) {
        const bool already = (max_num_segments != std::numeric_limits<int32_t>::max() && max_num_segments > 1 &&
                              eligible_count <= static_cast<size_t>(max_num_segments)) ||
                             (max_num_segments == 1 && eligible_count == 1);
        if (already) {
            message("already merged", context);
            return std::optional<MergeSpecification>{};
        }
    }

    if (force_merge_running) {
        // A force merge is already running; wait for it before planning another.
        return std::optional<MergeSpecification>{};
    }

    // Special case: merge everything into one segment.
    if (max_num_segments == 1 && total_merge_bytes < max_merge_bytes) {
        MergeSpecification spec;
        std::vector<const SegmentCommitInfo *> segments;
        segments.reserve(sorted_size_and_docs.value().size());
        for (const SegmentSizeAndDocs &segment : sorted_size_and_docs.value()) {
            segments.push_back(segment.seg_info);
        }
        spec.add(merge_of(segments));
        return std::optional<MergeSpecification>(std::move(spec));
    }

    std::optional<MergeSpecification> spec;
    size_t result_segments = eligible_count;
    // Pack the largest segments into merges of at most `max_merge_bytes`.
    std::ptrdiff_t index = static_cast<std::ptrdiff_t>(eligible_count) - 1;
    std::vector<const SegmentCommitInfo *> candidate;
    candidate.reserve(eligible_count);

    while (true) {
        candidate.clear();
        int64_t current_candidate_bytes = 0;

        while (index >= 0 && result_segments > static_cast<size_t>(max_num_segments)) {
            const SegmentSizeAndDocs &segment = sorted_size_and_docs.value()[static_cast<size_t>(index)];
            const size_t initial_candidate_size = candidate.size();
            const int64_t current_segment_size = segment.size_in_seg;
            // Add either because there is room, or because this is the smallest
            // possible bin (going further left only finds larger segments).
            if (current_candidate_bytes + current_segment_size <= max_merge_bytes || initial_candidate_size < 2) {
                candidate.push_back(segment.seg_info);
                --index;
                current_candidate_bytes += current_segment_size;
                if (initial_candidate_size > 0) {
                    // A merge of n segments removes n-1 of them.
                    --result_segments;
                }
            } else {
                break;
            }
        }

        if (candidate.size() > 1 && (!force_merge_running ||
                                     static_cast<double>(current_candidate_bytes) > 0.7 * static_cast<double>(max_merge_bytes))) {
            if (!spec.has_value()) {
                spec.emplace();
            }
            spec->add(merge_of(candidate));
        } else {
            return spec;
        }
    }
}

// ---------------------------------------------------------------------------
// find_forced_deletes_merges
// ---------------------------------------------------------------------------

Result<std::optional<MergeSpecification>> TieredMergePolicy::find_forced_deletes_merges(
        const SegmentInfos &infos, const MergeContext &context) const {
    const std::set<std::string> merging = context.get_merging_segments();

    // A quick check that there is any work to do.
    bool have_work = false;
    int32_t total_del_count = 0;
    for (const SegmentCommitInfo &info : infos.segments) {
        auto deleted = context.num_deletes_to_merge(info);
        if (!deleted.has_value()) {
            return std::unexpected(std::move(deleted.error()));
        }
        debug_check(deleted.value() >= 0 && deleted.value() <= info.max_doc(), "delete count out of range");
        total_del_count += deleted.value();

        const double pct_deletes = info.max_doc() == 0
                                           ? 0.0
                                           : 100.0 * static_cast<double>(deleted.value()) /
                                                     static_cast<double>(info.max_doc());
        have_work = have_work ||
                    (pct_deletes > force_merge_deletes_pct_allowed_ && !merging.contains(info.info.name));
    }

    if (!have_work) {
        return std::optional<MergeSpecification>{};
    }

    auto sorted_infos = get_sorted_by_segment_size(infos, context);
    if (!sorted_infos.has_value()) {
        return std::unexpected(std::move(sorted_infos.error()));
    }

    std::erase_if(sorted_infos.value(), [&](const SegmentSizeAndDocs &segment) {
        const double pct_deletes = segment.max_doc == 0
                                           ? 0.0
                                           : 100.0 * static_cast<double>(segment.del_count) /
                                                     static_cast<double>(segment.max_doc);
        return merging.contains(segment.seg_info->info.name) || pct_deletes <= force_merge_deletes_pct_allowed_;
    });

    return do_find_merges(std::move(sorted_infos.value()), max_merged_segment_bytes_,
                          std::numeric_limits<int32_t>::max(), std::numeric_limits<size_t>::max(), 0,
                          get_max_allowed_docs(static_cast<int32_t>(infos.total_max_doc()), total_del_count),
                          MergeType::ForceMergeDeletes, context, false);
}

} // namespace lucene
