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

export module lucene_core:segment_commit_info;

import :types;
import :error;

import std;

import :segment_info;

namespace lucene {

/// A [`SegmentInfo`] plus the state of the commit that references it.
///
/// Port of `SegmentCommitInfo` (`index/segment_commit_info.rs`).  Deletions,
/// doc-values updates and field-infos updates are recorded per commit, so a
/// segment can be shared by two commit points with different deletion counts.
export struct SegmentCommitInfo {
    /// The segment itself.
    SegmentInfo info;

    /// Generation of the `.liv` (deleted docs) file; `-1` when nothing was
    /// deleted yet, so the name falls back to the segment's own generation.
    int64_t del_gen{-1};
    /// Documents deleted by hard deletes.
    int32_t del_count{0};
    /// Documents deleted by soft deletes.
    int32_t soft_del_count{0};

    /// Generation of the `.fnm` file after a field-infos update, `-1` if none.
    int64_t field_infos_gen{-1};
    /// Generation of the `.dvd`/`.dvm` files after a doc-values update, `-1` if
    /// none.
    int64_t doc_values_gen{-1};

    /// Identity assigned to this commit's version of the segment (since 8.6).
    std::optional<std::vector<uint8_t>> id;

    /// Extra `.fnm` files kept alive by a field-infos update.
    std::set<std::string> field_infos_files;
    /// Doc-values update files, keyed by generation.
    std::map<int32_t, std::set<std::string>> doc_values_updates_files;

    [[nodiscard]] int32_t max_doc() const noexcept { return info.max_doc; }

    /// Documents that are alive in this commit.
    [[nodiscard]] int32_t doc_count() const noexcept { return info.max_doc - del_count - soft_del_count; }

    /// Total number of deletions, hard and soft.
    [[nodiscard]] int64_t total_del_count() const noexcept {
        return static_cast<int64_t>(del_count) + soft_del_count;
    }

    /// Generation the next live docs write has to use.
    [[nodiscard]] int64_t next_del_gen() const noexcept { return del_gen + 1; }

    /// `true` once a live docs file exists for this commit.
    [[nodiscard]] bool has_deletions() const noexcept { return del_gen != -1; }

    /// Checks the deletion counts against the segment size.
    [[nodiscard]] VoidResult validate() const;
};

} // namespace lucene
