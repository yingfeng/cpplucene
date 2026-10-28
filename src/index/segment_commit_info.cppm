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
    ///
    /// **Deviation:** the reference implementation keeps soft-deleted documents in
    /// the segment files and hides them when reading, so a retention merge policy
    /// can bring them back.  This port materializes deletes at commit time (see the
    /// class note), so a soft delete lands in the same `.liv` as a hard delete and
    /// the document is gone for good.  The two counters stay disjoint: together
    /// they cover every document the `.liv` hides.
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

    /// Generation the next `.fnm` write after a doc values update has to use.
    ///
    /// Generations start at `1`: `0` is the generation-less base file, whose name
    /// carries no suffix at all.
    [[nodiscard]] int64_t next_field_infos_gen() const noexcept {
        return field_infos_gen == -1 ? 1 : field_infos_gen + 1;
    }

    /// Marks the just-written `.fnm` generation as the current one.
    void advance_field_infos_gen() noexcept {
        field_infos_gen = next_field_infos_gen();
    }

    /// Generation the next doc values update has to use; see [`next_field_infos_gen`].
    [[nodiscard]] int64_t next_doc_values_gen() const noexcept {
        return doc_values_gen == -1 ? 1 : doc_values_gen + 1;
    }

    /// Marks the just-written doc values generation as the current one.
    void advance_doc_values_gen() noexcept {
        doc_values_gen = next_doc_values_gen();
    }

    /// Checks the deletion counts against the segment size.
    [[nodiscard]] VoidResult validate() const;
};

} // namespace lucene
