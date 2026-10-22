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

export module lucene_core:lucene90_points_format;

import :types;
import :error;

import std;

import :points_format;
import :points_reader;
import :points_writer;
import :segment_info;
import :segment_read_state;
import :segment_write_state;

namespace lucene {

/// Lucene 9.0 point format: dimensional values in a block KD-tree, for fast 1D
/// range and N-dimensional shape intersection filtering.
///
/// Port of `Lucene90PointsFormat`
/// (`codecs/lucene90/lucene90_points_format.rs`).  The data is spread over three
/// files:
///
///  * `.kdm`: the metadata of the fields (dimensions, bytes per dimension, the
///    tree shape and the bounds) plus the file pointers of the other two files;
///  * `.kdi`: the packed index, i.e. the inner nodes of the KD-tree;
///  * `.kdd`: the leaf blocks, where most of the indexed data lives.
///
/// **Deviation:** the reference implementation's `PointsFormat` trait ties the writer and reader types
/// to the format through associated types.  A C++ interface cannot do that, so
/// this struct exposes the two factories directly and returns the format-neutral
/// [`PointsWriter`]/[`PointsReader`] interfaces.
export struct Lucene90PointsFormat {
    /// Codec name of the leaf blocks file.
    static constexpr std::string_view DATA_CODEC_NAME = "Lucene90PointsFormatData";
    /// Codec name of the packed index file.
    static constexpr std::string_view INDEX_CODEC_NAME = "Lucene90PointsFormatIndex";
    /// Codec name of the metadata file.
    static constexpr std::string_view META_CODEC_NAME = "Lucene90PointsFormatMeta";

    /// Extension of the leaf blocks file.
    static constexpr std::string_view DATA_EXTENSION = "kdd";
    /// Extension of the per-field packed index.
    static constexpr std::string_view INDEX_EXTENSION = "kdi";
    /// Extension of the per-field metadata.
    static constexpr std::string_view META_EXTENSION = "kdm";

    /// First version this format ever wrote.
    static constexpr int32_t VERSION_START = 0;
    /// Version this port writes and reads (only the current one, by design).
    static constexpr int32_t VERSION_CURRENT = VERSION_START;

    /// Creates the writer for `state`, with Lucene's default leaf size (512) and
    /// a 16 MB in-heap sort budget.
    [[nodiscard]] static Result<std::unique_ptr<PointsWriter>> fields_writer(SegmentWriteState &state,
                                                                            const SegmentInfo &segment_info);

    /// Creates the reader for `state`.
    [[nodiscard]] static Result<std::unique_ptr<PointsReader>> fields_reader(SegmentReadState &state,
                                                                            const SegmentInfo &segment_info);
};

/// The Lucene90 points format as a [`PointsFormat`], so a codec can hand it out.
///
/// Port of the reference implementation's `impl PointsFormat for Lucene90PointsFormat`, which is a
/// unit struct; here the static helpers stay on the class and this adapter gives
/// them the virtual dispatch surface.
export class Lucene90PointsFormatAdapter final : public PointsFormat {
public:
    [[nodiscard]] Result<std::unique_ptr<PointsWriter>> fields_writer(SegmentWriteState &state,
                                                                      const SegmentInfo &segment_info) const override;

    [[nodiscard]] Result<std::unique_ptr<PointsReader>> fields_reader(SegmentReadState &state,
                                                                      const SegmentInfo &segment_info) const override;
};

} // namespace lucene
