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

export module lucene_core:segment_write_state;

import :types;
import std;

import :debug;
import :directory;
import :field_infos;
import :fixed_bit_set;
import :info_stream;
import :io_context;

namespace lucene {

/// Holder for the parameters shared by every writer of one segment.
///
/// Port of `SegmentWriteState` (`core/index/segment_write_state.rs`).  The
/// directory and the IO context are borrowed, the field infos and the live docs
/// are shared, exactly like the `Arc`s in the reference implementation.
export class SegmentWriteState {
public:
    /// Creates a state without a segment suffix.
    SegmentWriteState(Directory &directory, std::shared_ptr<const FieldInfos> field_infos,
                      const IOContext &context)
        : SegmentWriteState(get_default_info_stream(), directory, std::move(field_infos), context, {}) {}

    /// Creates a state with a segment suffix.
    SegmentWriteState(InfoStreamMT info_stream, Directory &directory,
                      std::shared_ptr<const FieldInfos> field_infos, const IOContext &context,
                      std::string segment_suffix)
        : info_stream(std::move(info_stream)),
          directory(directory),
          field_infos(std::move(field_infos)),
          segment_suffix(std::move(segment_suffix)),
          context(context) {
        debug_check(is_valid_segment_suffix(this->segment_suffix), "invalid segment suffix");
    }

    /// Copy of `state` with a different segment suffix (the reference implementation's
    /// `copy_with_suffix`).
    [[nodiscard]] static SegmentWriteState copy_with_suffix(const SegmentWriteState &state,
                                                           std::string segment_suffix) {
        SegmentWriteState copy(state.info_stream, state.directory, state.field_infos, state.context,
                               std::move(segment_suffix));
        copy.del_count_on_flush = state.del_count_on_flush;
        copy.soft_del_count_on_flush = state.soft_del_count_on_flush;
        copy.live_docs = state.live_docs;
        return copy;
    }

    /// A suffix is empty, `_X` (one underscore) or a base36 generation.
    ///
    /// A per-field format appends its own group, so a doc values update writes
    /// `<generation>_<format>_<format suffix>` (e.g. `1_Lucene90_0`): three parts
    /// are accepted as long as the first one, the generation, is not empty.
    ///
    /// The reference implementation keeps this check private and only runs it from a `debug_assert`;
    /// it is exported here so the writers using it can be tested directly.
    [[nodiscard]] static bool is_valid_segment_suffix(std::string_view suffix) {
        if (suffix.empty()) {
            return true;
        }
        for (const char c : suffix) {
            const bool allowed = c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                                 (c >= 'A' && c <= 'Z');
            if (!allowed) {
                return false;
            }
        }
        const size_t underscores = static_cast<size_t>(std::count(suffix.begin(), suffix.end(), '_'));
        if (underscores <= 1) {
            return true;
        }
        // `<generation>_<format>_<suffix>`, which always starts with the generation.
        return underscores == 2 && suffix.front() != '_';
    }

    /// Info stream used for debugging messages.
    InfoStreamMT info_stream;
    /// Directory the segment is written to.
    ///
    /// Non-const because `Directory::create_output` is non-const in this port
    /// (the reference implementation gets the same effect through interior mutability).
    Directory &directory;
    /// Field infos describing all fields in this segment.
    std::shared_ptr<const FieldInfos> field_infos;
    /// Number of deleted documents set while flushing the segment.
    int32_t del_count_on_flush{0};
    /// Number of only soft deleted documents set while flushing the segment.
    int32_t soft_del_count_on_flush{0};
    /// Live documents; only set when the segment has deletions.
    std::shared_ptr<const FixedBitSet> live_docs;
    /// Unique suffix of any postings files written for this segment.
    ///
    /// `PerFieldPostingsFormat` sets this for each of the postings formats it
    /// wraps, so every file written or read must be derived from it through
    /// `IndexFileNames::segment_file_name`.  The suffix must be empty, a textual
    /// suffix of exactly two parts separated by an underscore, or a base36
    /// generation.
    std::string segment_suffix;
    /// IO context for all writes; pass it to `Directory::create_output`.
    const IOContext &context;

private:
    /// `i64::from_str_radix(suffix, 36)` of the reference implementation: a non-empty run of
    /// `[0-9a-zA-Z]`.
    [[nodiscard]] static bool is_base36(std::string_view text) {
        if (text.empty()) {
            return false;
        }
        return std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        });
    }
};

} // namespace lucene
