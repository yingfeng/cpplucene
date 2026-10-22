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

export module lucene_core:lucene99_segment_info_format;

import :types;
import :error;

import std;

import :data_input;
import :data_output;
import :directory;
import :io_context;
import :segment_info;
import :segment_info_format;

namespace lucene {

/// Segment metadata format of the Lucene 9.9+ / 10 codec.
///
/// Port of `Lucene99SegmentInfoFormat`
/// (`codecs/lucene99/lucene99_segment_info_format.rs`).  The file is
/// `<segment>.si`:
///
/// ```text
/// header   // CodecUtil::write_index_header("Lucene90SegmentInfo", 0, id, "")
/// int32 major, minor, bugFix          // created version
/// byte  hasMinVersion; [int32 x3]     // minimum version, when present
/// int32 docCount
/// byte  isCompoundFile                // 1 == yes, 0 == no
/// byte  hasBlocks                     // 1 == yes, 0 == no
/// map   diagnostics
/// set   files
/// map   attributes
/// vint  numSortFields
/// footer
/// ```
///
/// **Deviation:** the reference implementation writes these ints with `DataOutput::write_int`, which is
/// little endian in both projects.  This port writes them big endian — Lucene's
/// own byte order — so a `.si` here matches a Java written one.  Maps, sets,
/// strings and vints are endian-independent either way.
///
/// The ints are big endian (Lucene's `DataOutput`), which is why this file uses
/// the `CodecUtil` BE helpers instead of the port's little endian
/// `write_int`/`read_int`.  Maps, sets, strings and vints are
/// endian-independent, so the file is byte-compatible with a Java written `.si`
/// as long as the sort field list is empty — which it always is here, because
/// this port has no `Sort` subsystem yet.
export class Lucene99SegmentInfoFormat : public SegmentInfoFormat {
public:
    static constexpr std::string_view CODEC_NAME = "Lucene90SegmentInfo";
    /// Only the current version is written and read; older headers are rejected
    /// as `index_format_too_old`.
    static constexpr int32_t VERSION_CURRENT = 0;
    /// Extension of the segment info file.
    static constexpr std::string_view SI_EXTENSION = "si";

    [[nodiscard]] std::string_view name() const override { return CODEC_NAME; }

    [[nodiscard]] VoidResult write(Directory &dir, SegmentInfo &info, const IOContext &context) const override;

    [[nodiscard]] Result<SegmentInfo> read(const Directory &dir, std::string_view segment,
                                           std::span<const uint8_t> id, const IOContext &context) const override;

    /// Writes the payload (everything between the header and the footer).
    [[nodiscard]] static VoidResult write_segment_info(DataOutput &out, const SegmentInfo &info);

    /// Reads the payload produced by [`write_segment_info`].
    [[nodiscard]] static Result<SegmentInfo> parse_segment_info(DataInput &input, std::string_view segment,
                                                                std::span<const uint8_t> id);
};

} // namespace lucene
