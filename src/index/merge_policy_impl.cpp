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

module lucene_core:merge_policy.impl;

import :merge_policy;

import std;

import :directory;
import :error;
import :info_stream;
import :segment_commit_info;
import :segment_infos;
import :string_helper;

namespace lucene {

// ---------------------------------------------------------------------------
// SimpleMergeContext
// ---------------------------------------------------------------------------

SimpleMergeContext::SimpleMergeContext(InfoStreamMT info_stream, Directory *directory)
        : info_stream_(std::move(info_stream)), directory_(directory) {}

Result<int32_t> SimpleMergeContext::num_deletes_to_merge(const SegmentCommitInfo &info) const {
    return static_cast<int32_t>(info.total_del_count());
}

Result<int32_t> SimpleMergeContext::num_deleted_docs(const SegmentCommitInfo &info) const {
    return static_cast<int32_t>(info.total_del_count());
}

void SimpleMergeContext::set_merging(std::string name, bool merging) {
    if (merging) {
        merging_.insert(std::move(name));
    } else {
        merging_.erase(name);
    }
}

// ---------------------------------------------------------------------------
// OneMerge
// ---------------------------------------------------------------------------

OneMerge::OneMerge(std::vector<SegmentCommitInfo> segments) : segments_(std::move(segments)) {}

int64_t OneMerge::total_max_doc() const noexcept {
    int64_t total = 0;
    for (const auto &segment : segments_) {
        total += segment.max_doc();
    }
    return total;
}

int64_t OneMerge::total_doc_count(const MergeContext &context) const {
    int64_t total = 0;
    for (const auto &segment : segments_) {
        auto deleted = context.num_deleted_docs(segment);
        if (!deleted.has_value()) {
            // Callers only log with this; falling back to the on-disk count is
            // the best that can be done without a way to report the failure.
            total += segment.doc_count();
            continue;
        }
        total += static_cast<int64_t>(segment.max_doc()) - deleted.value();
    }
    return total;
}

std::string OneMerge::to_string() const {
    std::string text = "OneMerge(segments=[";
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (i != 0) {
            text += ", ";
        }
        const SegmentCommitInfo &segment = segments_[i];
        text += "segment(";
        text += "name=";
        text += segment.info.name;
        text += " id=";
        text += StringHelper::id_to_string(segment.info.id);
        text += " maxDoc=";
        text += std::to_string(segment.max_doc());
        text += " delCount=";
        text += std::to_string(segment.del_count);
        text += " softDelCount=";
        text += std::to_string(segment.soft_del_count);
        text += ")";
    }
    text += "])";
    return text;
}

// ---------------------------------------------------------------------------
// MergePolicy
// ---------------------------------------------------------------------------

Result<std::optional<MergeSpecification>> MergePolicy::find_forced_merges(const SegmentInfos & /*infos*/,
                                                                          int32_t /*max_num_segments*/,
                                                                          const MergeContext & /*context*/) const {
    return std::optional<MergeSpecification>{};
}

Result<std::optional<MergeSpecification>> MergePolicy::find_forced_deletes_merges(
        const SegmentInfos & /*infos*/, const MergeContext & /*context*/) const {
    return std::optional<MergeSpecification>{};
}

bool MergePolicy::use_compound_file(const SegmentInfos & /*infos*/, const SegmentCommitInfo & /*merged_info*/,
                                   const MergeContext & /*context*/) const {
    // This port never writes compound files, so a merge never produces one.
    return false;
}

VoidResult MergePolicy::set_no_cfs_ratio(double ratio) {
    if (ratio < 0.0 || ratio > 1.0) {
        return err(LuceneError::illegal_argument(
                std::format("noCFSRatio must be 0.0 to 1.0 inclusive; got {}", ratio)));
    }
    no_cfs_ratio_ = ratio;
    return {};
}

double MergePolicy::max_cfs_segment_size_mb() const noexcept {
    return static_cast<double>(max_cfs_segment_size_) / 1024.0 / 1024.0;
}

VoidResult MergePolicy::set_max_cfs_segment_size_mb(double megabytes) {
    if (megabytes < 0.0) {
        return err(LuceneError::illegal_argument(std::format("maxCFSSegmentSizeMB must be >=0 (got {})", megabytes)));
    }
    const double bytes = megabytes * 1024.0 * 1024.0;
    max_cfs_segment_size_ = bytes > static_cast<double>(std::numeric_limits<int64_t>::max())
                                    ? std::numeric_limits<int64_t>::max()
                                    : static_cast<int64_t>(bytes);
    return {};
}

Result<bool> MergePolicy::has_merged(const SegmentInfos &infos, const SegmentCommitInfo &info,
                                    const MergeContext &context) const {
    auto deleted = deletes_to_merge(info, context);
    if (!deleted.has_value()) {
        return std::unexpected(std::move(deleted.error()));
    }
    debug_check(deleted.value() >= 0 && deleted.value() <= info.max_doc(), "delete count out of range");
    return deleted.value() == 0 && use_compound_file(infos, info, context) == info.info.use_compound_file;
}

Result<std::string> MergePolicy::seg_string(const MergeContext &context,
                                            std::span<const SegmentCommitInfo> infos) const {
    std::string text;
    for (size_t i = 0; i < infos.size(); ++i) {
        if (i != 0) {
            text += " ";
        }
        auto deleted = context.num_deleted_docs(infos[i]);
        if (!deleted.has_value()) {
            return std::unexpected(std::move(deleted.error()));
        }
        // `deleted` includes the documents already marked as deleted on disk, so
        // the difference is what is still pending.
        const int32_t pending = deleted.value() - infos[i].del_count;
        text += "segment(name=";
        text += infos[i].info.name;
        text += " maxDoc=";
        text += std::to_string(infos[i].max_doc());
        text += " delCount=";
        text += std::to_string(infos[i].del_count);
        text += " pendingDeletes=";
        text += std::to_string(pending);
        text += ")";
    }
    return text;
}

void MergePolicy::message(std::string_view message_text, const MergeContext &context) const {
    if (!verbose_) {
        return;
    }
    const InfoStreamMT stream = context.get_info_stream();
    if (stream == nullptr) {
        return;
    }
    // Logging must not fail the merge, so the result is dropped.
    (void)stream->message("MP", message_text);
}

Result<int32_t> MergePolicy::deletes_to_merge(const SegmentCommitInfo &info, const MergeContext &context) const {
    return context.num_deletes_to_merge(info);
}

// ---------------------------------------------------------------------------
// NoMergePolicy
// ---------------------------------------------------------------------------

std::shared_ptr<NoMergePolicy> NoMergePolicy::create() { return std::make_shared<NoMergePolicy>(); }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Result<int64_t> segment_size_in_bytes(const SegmentCommitInfo &info, const MergeContext &context) {
    Directory *directory = context.directory();
    if (directory == nullptr) {
        return err(LuceneError::unsupported_operation("the merge context has no directory to measure segment sizes"));
    }
    int64_t total = 0;
    for (const std::string &file : info.info.files) {
        auto length = directory->file_length(file);
        if (!length.has_value()) {
            return std::unexpected(std::move(length.error()));
        }
        total += static_cast<int64_t>(length.value());
    }
    return total;
}

Result<int64_t> calibrated_size_in_bytes(const SegmentCommitInfo &info, const MergeContext &context) {
    auto bytes = segment_size_in_bytes(info, context);
    if (!bytes.has_value()) {
        return std::unexpected(std::move(bytes.error()));
    }
    const int32_t max_doc = info.max_doc();
    if (max_doc <= 0) {
        return bytes.value();
    }
    auto deleted = context.num_deletes_to_merge(info);
    if (!deleted.has_value()) {
        return std::unexpected(std::move(deleted.error()));
    }
    debug_check(deleted.value() >= 0 && deleted.value() <= max_doc, "delete count out of range");
    const double del_ratio = static_cast<double>(deleted.value()) / static_cast<double>(max_doc);
    return static_cast<int64_t>(static_cast<double>(bytes.value()) * (1.0 - del_ratio));
}

} // namespace lucene
