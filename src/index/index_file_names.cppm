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

export module lucene_core:index_file_names;

import :types;
import :error;

import std;

namespace lucene {

/// Composition and parsing of index file names.
///
/// Port of `index/index_file_names.rs`.  Only the parts the commit machinery
/// needs are here: generation based names (`segments_7`), plain names, and the
/// helpers that strip an extension or a generation.
export struct IndexFileNames {
    /// Base name of the commit file.
    static constexpr std::string_view SEGMENTS = "segments";
    /// Base name of the file written while a commit is in flight.
    static constexpr std::string_view PENDING_SEGMENTS = "pending_segments";
    /// The pre-7.0 generation pointer, which this port only recognises so it can
    /// ignore it.
    static constexpr std::string_view OLD_SEGMENTS_GEN = "segments.gen";

    /// `<base>_<base36 generation><extension>`; `pending_segments` has no
    /// generation.
    [[nodiscard]] static std::string file_name_from_generation(std::string_view base, std::string_view extension,
                                                               int64_t generation);

    /// `<base>.<extension>` with either part optional.
    [[nodiscard]] static std::string file_name(std::string_view base, std::string_view extension);

    /// Generation encoded in `the file name`, or an error when `name` is not a
    /// `base_<generation>` file.
    [[nodiscard]] static Result<int64_t> generation_from_segment_file_name(std::string_view name,
                                                                          std::string_view base);

    /// `name` without its final `.extension`, or `name` when it has none.
    [[nodiscard]] static std::string strip_extension(std::string_view name);

    /// `name` without the `_<generation>` suffix of `base` files.
    [[nodiscard]] static std::string strip_segment_name(std::string_view name, std::string_view base);

    /// Base 36 encoding, as used for generations in file names.
    [[nodiscard]] static std::string base36_encode(int64_t value);

    /// Inverse of [`base36_encode`].
    [[nodiscard]] static Result<int64_t> base36_decode(std::string_view text);
};

} // namespace lucene
