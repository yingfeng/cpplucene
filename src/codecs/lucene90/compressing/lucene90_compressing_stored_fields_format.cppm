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

export module lucene_core:lucene90_compressing_stored_fields_format;

import :types;
import std;

import :compression;
import :data_input;
import :data_output;
import :directory;
import :error;
import :field_infos;
import :io_context;
import :segment_info;
import :stored_fields_format;
import :stored_fields_reader;
import :stored_fields_writer;

namespace lucene {

// ---- file layout ----
//
// Ported from `codecs/lucene90/compressing/lucene90_compressing_stored_fields_writer.rs`.

/// The documents, chunk by chunk, compressed.
export inline constexpr std::string_view STORED_FIELDS_FIELDS_EXTENSION = "fdt";
/// The chunk index.
export inline constexpr std::string_view STORED_FIELDS_INDEX_EXTENSION = "fdx";
/// The chunk size and the dirty-chunk statistics.
export inline constexpr std::string_view STORED_FIELDS_META_EXTENSION = "fdm";
export inline constexpr std::string_view STORED_FIELDS_INDEX_CODEC_NAME = "Lucene90FieldsIndex";
export inline constexpr std::string_view STORED_FIELDS_META_CODEC_NAME = "Lucene90FieldsIndexMeta";

/// Type codes stored next to the field number, one per value type.
export inline constexpr int32_t SF_STRING = 0x00;
export inline constexpr int32_t SF_BYTE_ARR = 0x01;
export inline constexpr int32_t SF_NUMERIC_INT = 0x02;
export inline constexpr int32_t SF_NUMERIC_FLOAT = 0x03;
export inline constexpr int32_t SF_NUMERIC_LONG = 0x04;
export inline constexpr int32_t SF_NUMERIC_DOUBLE = 0x05;

/// Bits the field number is shifted by, leaving room for the type code.
/// `unsigned_bits_required(NUMERIC_DOUBLE) == 3`.
export inline constexpr int32_t SF_TYPE_BITS = 3;
export inline constexpr int64_t SF_TYPE_MASK = (int64_t{1} << SF_TYPE_BITS) - 1;

export inline constexpr int32_t STORED_FIELDS_VERSION_START = 1;
/// Only the current version is written and read, by design.
export inline constexpr int32_t STORED_FIELDS_VERSION_CURRENT = STORED_FIELDS_VERSION_START;
/// The `.fdm` version, independent of the `.fdt` version.
export inline constexpr int32_t STORED_FIELDS_META_VERSION_START = 0;

// ---- z-encodings ----
//
// Ported from the helpers at the bottom of the writer: they trade a few branch
// instructions for a much shorter encoding of the values that dominate stored
// fields (small integers, timestamps and floats).

/// Writes a float in 1 to 5 bytes.
[[nodiscard]] VoidResult write_zfloat(DataOutput &out, float f);
/// Reads a float written by [`write_zfloat`].
[[nodiscard]] Result<float> read_zfloat(DataInput &in);

/// Writes a double in 1 to 9 bytes.
[[nodiscard]] VoidResult write_zdouble(DataOutput &out, double d);
/// Reads a double written by [`write_zdouble`].
[[nodiscard]] Result<double> read_zdouble(DataInput &in);

/// Writes a long in 1 to 10 bytes, with a special case for timestamps that are a
/// multiple of a second, an hour or a day.
[[nodiscard]] VoidResult write_tlong(DataOutput &out, int64_t value);
/// Reads a long written by [`write_tlong`].
[[nodiscard]] Result<int64_t> read_tlong(DataInput &in);

/// A stored fields format that compresses documents in chunks.
///
/// Port of `Lucene90CompressingStoredFieldsFormat`
/// (`codecs/lucene90/compressing/lucene90_compressing_stored_fields_format.rs`).
/// Documents are concatenated until the buffer reaches `chunk_size` bytes or
/// `max_docs_per_chunk` documents, then the buffer is compressed as a whole,
/// which is what makes the ratio much better than compressing per document.
export class Lucene90CompressingStoredFieldsFormat final : public StoredFieldsFormat {
public:
    /// Creates a format, validating the chunk parameters.
    [[nodiscard]] static Result<Lucene90CompressingStoredFieldsFormat> create(
            std::string format_name, CompressionMode compression_mode, int32_t chunk_size,
            int32_t max_docs_per_chunk, int32_t block_shift);

    [[nodiscard]] std::string_view name() const override { return format_name_; }

    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> fields_reader(
            const Directory &directory, const SegmentInfo &segment_info,
            std::shared_ptr<const FieldInfos> field_infos, const IOContext &context) const override;

    [[nodiscard]] Result<std::unique_ptr<StoredFieldsWriter>> fields_writer(
            Directory &directory, const SegmentInfo &segment_info, const IOContext &context) const override;

    [[nodiscard]] const std::string &format_name() const noexcept { return format_name_; }
    [[nodiscard]] const CompressionMode &compression_mode() const noexcept { return compression_mode_; }
    [[nodiscard]] int32_t chunk_size() const noexcept { return chunk_size_; }
    [[nodiscard]] int32_t max_docs_per_chunk() const noexcept { return max_docs_per_chunk_; }
    [[nodiscard]] int32_t block_shift() const noexcept { return block_shift_; }

private:
    Lucene90CompressingStoredFieldsFormat(std::string format_name, CompressionMode compression_mode,
                                          int32_t chunk_size, int32_t max_docs_per_chunk, int32_t block_shift)
        : format_name_(std::move(format_name)),
          compression_mode_(compression_mode),
          chunk_size_(chunk_size),
          max_docs_per_chunk_(max_docs_per_chunk),
          block_shift_(block_shift) {}

    std::string format_name_;
    CompressionMode compression_mode_;
    int32_t chunk_size_{0};
    int32_t max_docs_per_chunk_{0};
    int32_t block_shift_{0};
};

} // namespace lucene
