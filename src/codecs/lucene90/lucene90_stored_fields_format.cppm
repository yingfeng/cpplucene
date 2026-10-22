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

export module lucene_core:lucene90_stored_fields_format;

import :types;
import std;

import :error;
import :field_infos;
import :io_context;
import :lucene90_compressing_stored_fields_format;
import :segment_info;
import :stored_fields_format;
import :stored_fields_reader;
import :stored_fields_writer;

namespace lucene {

/// Lucene 9.0 stored fields format: chunked, compressed blocks of documents.
///
/// Port of `Lucene90StoredFieldsFormat`
/// (`core/codecs/lucene90/lucene90_stored_fields_format.rs`).  It is a thin
/// wrapper that picks the compression parameters of a
/// [`Lucene90CompressingStoredFieldsFormat`] from a [`Mode`] and records the mode
/// as a segment attribute, so a reader can rebuild the same parameters.
///
/// **Deviations:**
///  * `Mode::BestCompression` is **not** available: it needs the Deflate
///    compression mode, which this port deliberately does not ship (see
///    `lucene_core:compression`).  Asking for it fails with
///    `unsupported_operation` instead of silently writing LZ4 data under the
///    "high" format name;
///  * the mode is read and written through `SegmentInfo::attributes` directly
///    (rlucene has `get_attribute` / `put_attribute` helpers on `SegmentInfo`).
export class Lucene90StoredFieldsFormat final : public StoredFieldsFormat {
public:
    /// Compression mode of the stored fields.
    enum class Mode : uint8_t {
        /// Trade compression ratio for retrieval speed (LZ4, 10 x 8 KB blocks).
        BestSpeed,
        /// Trade retrieval speed for compression ratio (Deflate, 10 x 48 KB
        /// blocks); not ported, see the class notes.
        BestCompression,
    };

    /// Attribute key under which the mode is persisted in the segment metadata.
    static constexpr std::string_view MODE_KEY = "Lucene90StoredFieldsFormat.mode";
    /// Shoot for 10 sub blocks of 48 KB each (`BestCompression`).
    static constexpr int32_t BEST_COMPRESSION_BLOCK_LENGTH = 10 * 48 * 1024;
    /// Shoot for 10 sub blocks of 8 KB each (`BestSpeed`).
    static constexpr int32_t BEST_SPEED_BLOCK_LENGTH = 10 * 8 * 1024;

    /// The format name reported to the codec.
    static constexpr std::string_view FORMAT_NAME = "Lucene90StoredFields";

    /// Stored fields format with the default options.
    Lucene90StoredFieldsFormat() = default;
    /// Stored fields format using `mode`.
    explicit Lucene90StoredFieldsFormat(Mode mode) : mode_(mode) {}

    [[nodiscard]] Mode mode() const noexcept { return mode_; }

    /// The persisted name of `mode` (`"BEST_SPEED"` / `"BEST_COMPRESSION"`).
    [[nodiscard]] static std::string_view mode_name(Mode mode);
    /// Parses a persisted mode name.
    [[nodiscard]] static Result<Mode> mode_from_name(std::string_view name);

    [[nodiscard]] std::string_view name() const override { return FORMAT_NAME; }

    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> fields_reader(
            const Directory &directory, const SegmentInfo &segment_info,
            std::shared_ptr<const FieldInfos> field_infos, const IOContext &context) const override;

    /// Records the mode in `segment_info` and returns the writer of the
    /// matching compressing format.
    [[nodiscard]] Result<std::unique_ptr<StoredFieldsWriter>> fields_writer(
            Directory &directory, SegmentInfo &segment_info, const IOContext &context) const override;

    /// The compressing format that implements `mode`.
    [[nodiscard]] static Result<Lucene90CompressingStoredFieldsFormat> format_for_mode(Mode mode);

private:
    Mode mode_{Mode::BestSpeed};
};

} // namespace lucene
