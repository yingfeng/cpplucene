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

export module lucene_core:segment_info;

import :types;
import :error;
import :sort_field;
import :version;

import std;

namespace lucene {

/// Length of a segment id (`StringHelper::ID_LENGTH`).
///
/// The id ends up in every codec header of the segment, so it has to be the same
/// length Lucene uses.
export constexpr size_t ID_LENGTH = 16;

/// Longest allowed segment name, so `name + file extension` fits a file name.
export constexpr size_t MAX_SEGMENT_NAME_LENGTH = 60;

/// Splits a file name into its segment part.
///
/// Port of `IndexFileNames::parseSegmentName`: the segment name is everything
/// before the first `.`.  A file name with no dot is its own segment name, which
/// is how Lucene treats e.g. `segments_1`.
export [[nodiscard]] Result<std::string> parse_segment_name(std::string_view file_name);

/// Builds `<segment>_<suffix>.<extension>` the way `IndexFileNames::segmentFileName`
/// does: the underscore and the suffix are both optional.
export [[nodiscard]] std::string segment_file_name(std::string_view segment, std::string_view suffix,
                                                   std::string_view extension);

/// Checks that `file_name` belongs to `segment`, returning a readable error when
/// it does not.
export [[nodiscard]] VoidResult check_file_name(std::string_view file_name, std::string_view segment);

/// Metadata about one segment of an index.
///
/// Port of `SegmentInfo` (`index/segment_info.rs`), reduced to what the codecs
/// need: the reference implementation keeps the directory, the codec and the `Sort` object alive
/// inside it, while this port keeps plain data and leaves the wiring to the
/// caller.  That keeps the type copyable and free of ownership cycles.
export struct SegmentInfo {
    /// Segment name (`_0`, `_1`, ...); the `.si` file is `<name>.si`.
    std::string name;
    /// `ID_LENGTH` byte identity, also embedded into every codec header.
    std::vector<uint8_t> id;
    /// Version of the code that created the segment.
    std::optional<Version> version;
    /// Oldest version of the code that contributed documents, since 7.0.
    std::optional<Version> min_version;
    int32_t max_doc{0};
    /// `true` when the segment's files are packed into a `.cfs` file.
    bool use_compound_file{false};
    /// `true` when documents were indexed as blocks (`addDocumentBlock`).
    bool has_blocks{false};
    /// Codec that owns the segment, as recorded in `segments_N`.
    std::string codec_name{"Lucene99"};
    std::map<std::string, std::string> diagnostics;
    /// Files the segment consists of.
    std::set<std::string> files;
    /// Free-form attributes; `Lucene90` uses one entry to store the
    /// `softDeletesField` name.
    std::map<std::string, std::string> attributes;
    /// The index sort the segment was written with, when it has one.
    ///
    /// The documents of a sorted segment are in the order this describes, which
    /// is what lets the search side skip sorting and stop early.
    std::optional<Sort> sort;

    /// Validates the identity fields without touching the file names.
    [[nodiscard]] VoidResult validate() const;

    /// Adds `file` to [`files`], after checking that it belongs to this segment.
    [[nodiscard]] VoidResult add_file(std::string_view file);

    /// Replaces the file set, after checking every name.
    [[nodiscard]] VoidResult set_files(std::set<std::string> new_files);

    [[nodiscard]] std::string si_file_name() const { return segment_file_name(name, "", "si"); }
};

} // namespace lucene
