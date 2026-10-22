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

export module lucene_core:lucene90_compressing_term_vectors_format;

import :types;
import std;

import :compression;
import :directory;
import :error;
import :field_infos;
import :io_context;
import :segment_info;
import :term_vectors_format;
import :term_vectors_reader;
import :term_vectors_writer;

namespace lucene {

/// Extension of the compressed term-vectors data (`.tvd`).
inline constexpr std::string_view VECTORS_EXTENSION = "tvd";
/// Extension of the chunk index (`.tvx`).
inline constexpr std::string_view VECTORS_INDEX_EXTENSION = "tvx";
/// Extension of the metadata (`.tvm`).
inline constexpr std::string_view VECTORS_META_EXTENSION = "tvm";
/// Codec name of the chunk index.
inline constexpr std::string_view VECTORS_INDEX_CODEC_NAME = "Lucene90TermVectorsIndex";
/// Codec name of the metadata file.
inline constexpr std::string_view VECTORS_META_CODEC_NAME = "Lucene90TermVectorsIndexMeta";

/// Only the current format version is written and read.
inline constexpr int32_t VERSION_START = 0;
inline constexpr int32_t VERSION_CURRENT = VERSION_START;
inline constexpr int32_t META_VERSION_START = 0;

/// Block size of the `BlockPackedWriter` that encodes lengths, frequencies and
/// positions.
inline constexpr size_t PACKED_BLOCK_SIZE = 64;

/// The field has positions.
inline constexpr int32_t TV_POSITIONS = 0x01;
/// The field has offsets.
inline constexpr int32_t TV_OFFSETS = 0x02;
/// The field has payloads.
inline constexpr int32_t TV_PAYLOADS = 0x04;

/// Term vectors stored in compressed chunks, sharing the chunk index and the
/// compression of `Lucene90CompressingStoredFieldsFormat`.
///
/// Port of `Lucene90CompressingTermVectorsFormat`
/// (`codecs/lucene90/compressing/lucene90_compressing_term_vectors_format.rs`).
///
/// Three files are written:
///
/// ```text
/// .tvd  header "Lucene90TermVectors", chunks of compressed term vectors
/// .tvx  header "Lucene90TermVectorsIndex" + header "Lucene90TermVectorsIndexMeta",
///       the chunk index (doc -> pointer/length)
/// .tvm  header "Lucene90TermVectorsIndexMeta", packed-ints version, chunk size,
///       the index metadata, chunk statistics, footer
/// ```
export class Lucene90CompressingTermVectorsFormat final : public TermVectorsFormat {
public:
    /// Creates the format.
    ///
    /// * `format_name` — written into the `.tvd` header so a reader can reject a
    ///   segment written by a different variant;
    /// * `segment_suffix` — the per-file suffix of the segment (`""` in practice);
    /// * `chunk_size` — minimum number of bytes of one chunk;
    /// * `max_docs_per_chunk` — maximum number of documents of one chunk;
    /// * `block_size` — number of chunks per index block.
    [[nodiscard]] static Result<Lucene90CompressingTermVectorsFormat> create(std::string format_name,
                                                                            std::string segment_suffix,
                                                                            CompressionMode compression_mode,
                                                                            int32_t chunk_size,
                                                                            int32_t max_docs_per_chunk,
                                                                            int32_t block_size);

    [[nodiscard]] std::string_view name() const override { return format_name_; }

    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> vectors_reader(
            const Directory &directory, const SegmentInfo &segment_info,
            std::shared_ptr<const FieldInfos> field_infos, const IOContext &context) const override;

    [[nodiscard]] Result<std::unique_ptr<TermVectorsWriter>> vectors_writer(
            Directory &directory, const SegmentInfo &segment_info, const IOContext &context) const override;

    [[nodiscard]] const std::string &format_name() const noexcept { return format_name_; }
    [[nodiscard]] const std::string &segment_suffix() const noexcept { return segment_suffix_; }
    [[nodiscard]] const CompressionMode &compression_mode() const noexcept { return compression_mode_; }
    [[nodiscard]] int32_t chunk_size() const noexcept { return chunk_size_; }
    [[nodiscard]] int32_t max_docs_per_chunk() const noexcept { return max_docs_per_chunk_; }
    [[nodiscard]] int32_t block_size() const noexcept { return block_size_; }

    /// Same text as rlucene's `Display` implementation.
    [[nodiscard]] std::string to_string() const;

private:
    Lucene90CompressingTermVectorsFormat(std::string format_name, std::string segment_suffix,
                                         CompressionMode compression_mode, int32_t chunk_size,
                                         int32_t max_docs_per_chunk, int32_t block_size);

    std::string format_name_;
    std::string segment_suffix_;
    CompressionMode compression_mode_;
    int32_t chunk_size_{0};
    int32_t max_docs_per_chunk_{0};
    int32_t block_size_{0};
};

} // namespace lucene
