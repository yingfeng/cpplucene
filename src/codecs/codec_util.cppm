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

export module lucene_core:codec_util;

import :types;
import :error;

import std;

import :data_input;
import :data_output;
import :index_input;
import :index_output;

namespace lucene {

/// Codec file headers and footers (`CodecHeader` / `CodecFooter`).
///
/// Port of `codecs/codec_util.rs`.  Every Lucene file starts with
/// `magic, codecName, version` and ends with `magic, algorithmId, crc32`, all
/// integers big endian.
export struct CodecUtil {
    /// Identifies the start of a codec header (`0x3fd76c17`).
    static constexpr int32_t CODEC_MAGIC = 0x3fd76c17;
    /// Identifies the start of a codec footer (`!CODEC_MAGIC`).
    static constexpr int32_t FOOTER_MAGIC = ~CODEC_MAGIC;

    /// Writes `magic, codec, version`.
    [[nodiscard]] static VoidResult write_header(DataOutput &out, std::string_view codec, int32_t version);

    /// Reads and validates a header written by [`write_header`].
    [[nodiscard]] static Result<int32_t> check_header(DataInput &input, std::string_view codec, int32_t min_version,
                                                      int32_t max_version);

    /// Like [`check_header`] but assumes the magic was already consumed.
    [[nodiscard]] static Result<int32_t> check_header_no_magic(DataInput &input, std::string_view codec,
                                                               int32_t min_version, int32_t max_version);

    /// Length of a header for `codec` (whose name must be shorter than 128
    /// characters, so the name length is a single-byte vint).
    [[nodiscard]] static size_t header_length(std::string_view codec);

    /// Length of a segment id embedded in an index header.
    static constexpr size_t ID_LENGTH = 16;

    /// Writes `magic, codec, version, id, suffix`.
    ///
    /// Port of `CodecUtil::writeIndexHeader`.  Segment-level files carry the
    /// segment id and a per-file suffix so a reader can tell which segment (and
    /// which generation of a file) it opened.
    [[nodiscard]] static VoidResult write_index_header(DataOutput &out, std::string_view codec, int32_t version,
                                                      std::span<const uint8_t> id, std::string_view suffix);

    /// Reads and validates an index header, including the `id` and `suffix`.
    [[nodiscard]] static Result<int32_t> check_index_header(DataInput &input, std::string_view codec,
                                                           int32_t min_version, int32_t max_version,
                                                           std::span<const uint8_t> id, std::string_view suffix);

    /// Copies the index header of `input` to `output`, validating that the
    /// segment id is `expected_id`.
    ///
    /// Used when packing files into a compound file: the sub-file keeps its own
    /// header (and therefore its own id), so the reader can still tell which
    /// segment it came from.
    ///
    /// Port of `CodecUtil::verifyAndCopyIndexHeader`.
    [[nodiscard]] static VoidResult verify_and_copy_index_header(IndexInput &input, DataOutput &output,
                                                                 std::span<const uint8_t> expected_id);

    /// Writes `magic, algorithmId, crc32` where the CRC covers everything
    /// written so far.
    [[nodiscard]] static VoidResult write_footer(IndexOutput &out);

    /// Length of a footer.
    [[nodiscard]] static constexpr size_t footer_length() noexcept { return 16; }

    /// Reads the footer structure and returns the stored CRC.
    ///
    /// **Deviation:** rlucene takes a `ChecksumIndexInput` that recomputes the
    /// CRC while reading; clucene has no checksum-tracking input yet, so this
    /// overload only validates the structure.  Use the two-argument overload to
    /// actually compare against a checksum.
    [[nodiscard]] static Result<int64_t> check_footer(DataInput &input);

    /// Validates the footer structure and compares the stored CRC with
    /// `actual_checksum`.
    [[nodiscard]] static Result<int64_t> check_footer(DataInput &input, int64_t actual_checksum);

    /// Seeks to `length - footer_length()` and returns the stored CRC without
    /// comparing it.
    [[nodiscard]] static Result<int64_t> retrieve_checksum(IndexInput &input);

    /// Same as [`retrieve_checksum`], but first verifies that the file length is
    /// exactly `expected_length` (rlucene's `retrieve_checksum_with_expected`).
    [[nodiscard]] static Result<int64_t> retrieve_checksum_with_expected(IndexInput &input,
                                                                        size_t expected_length);

    /// Reads the whole file and verifies that its CRC32 matches the footer.
    ///
    /// The input is left untouched (a clone is checksummed), which is what
    /// `check_integrity` implementations need.
    [[nodiscard]] static Result<int64_t> checksum_entire_file(IndexInput &input);

    /// Reads a big-endian `int32_t`.
    [[nodiscard]] static Result<int32_t> read_be_int(DataInput &input);
    /// Reads a big-endian `int64_t`.
    [[nodiscard]] static Result<int64_t> read_be_long(DataInput &input);
    /// Writes a big-endian `int32_t`.
    [[nodiscard]] static VoidResult write_be_int(DataOutput &out, int32_t value);
    /// Writes a big-endian `int64_t`.
    [[nodiscard]] static VoidResult write_be_long(DataOutput &out, int64_t value);

    /// Reads the CRC from a footer, rejecting values with the upper 32 bits set.
    [[nodiscard]] static Result<int64_t> read_crc(IndexInput &input);
    /// Writes the output's current CRC as a big-endian `int64_t`.
    [[nodiscard]] static VoidResult write_crc(IndexOutput &out);

private:
    /// Validates `magic, algorithmId`.
    [[nodiscard]] static VoidResult validate_footer(DataInput &input);
};

} // namespace lucene
