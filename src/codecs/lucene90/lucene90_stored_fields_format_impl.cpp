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

module lucene_core:lucene90_stored_fields_format.impl;

import :lucene90_stored_fields_format;

import :compression;
import :error;
import :field_infos;
import :io_context;
import :lucene90_compressing_stored_fields_format;
import :segment_info;
import :stored_fields_reader;
import :stored_fields_writer;
import :types;

import std;

namespace lucene {

namespace {

/// Format name recorded in the `.fdt` header of the fast (plain LZ4) variant.
constexpr std::string_view FAST_DATA_FORMAT_NAME = "Lucene90StoredFieldsFastData";

/// Format name of Lucene's default `BEST_SPEED` variant: LZ4 with a preset
/// dictionary.  Segments written before this port had that mode carry
/// [`FAST_DATA_FORMAT_NAME`] instead, which `fields_reader` still accepts.
constexpr std::string_view BEST_SPEED_DATA_FORMAT_NAME = "Lucene90StoredFieldsData";

/// Lucene's `BEST_SPEED` parameters: 10 x 8 KB sub blocks, 1024 documents or
/// 10 x 8 KB bytes per chunk, 10 chunks per index block.
constexpr int32_t BEST_SPEED_CHUNK_SIZE = 10 * 8 * 1024;
constexpr int32_t BEST_SPEED_MAX_DOCS_PER_CHUNK = 1024;
constexpr int32_t BEST_SPEED_BLOCK_SHIFT = 10;

} // namespace

std::string_view Lucene90StoredFieldsFormat::mode_name(Mode mode) {
    switch (mode) {
        case Mode::BestSpeed:
            return "BEST_SPEED";
        case Mode::BestCompression:
            return "BEST_COMPRESSION";
    }
    return "BEST_SPEED";
}

Result<Lucene90StoredFieldsFormat::Mode> Lucene90StoredFieldsFormat::mode_from_name(std::string_view name) {
    if (name == "BEST_SPEED") {
        return Mode::BestSpeed;
    }
    if (name == "BEST_COMPRESSION") {
        return Mode::BestCompression;
    }
    return err(LuceneError::illegal_state("unknown mode name: " + std::string(name)));
}

Result<Lucene90CompressingStoredFieldsFormat> Lucene90StoredFieldsFormat::format_for_mode(Mode mode) {
    switch (mode) {
        case Mode::BestSpeed:
            return Lucene90CompressingStoredFieldsFormat::create(std::string(BEST_SPEED_DATA_FORMAT_NAME),
                                                                 CompressionMode::best_speed(),
                                                                 BEST_SPEED_CHUNK_SIZE,
                                                                 BEST_SPEED_MAX_DOCS_PER_CHUNK,
                                                                 BEST_SPEED_BLOCK_SHIFT);
        case Mode::BestCompression:
            // The high-compression variant is Deflate-based; the compressor is
            // not part of this port, so refuse rather than write LZ4 bytes under
            // the "high" format name.
            return err(LuceneError::unsupported_operation(
                    "Lucene90StoredFieldsFormat: Mode::BestCompression needs the Deflate compression mode, "
                    "which is not ported"));
    }
    return err(LuceneError::illegal_argument("unknown stored fields mode"));
}

Result<std::unique_ptr<StoredFieldsReader>> Lucene90StoredFieldsFormat::fields_reader(
        const Directory &directory, const SegmentInfo &segment_info, std::shared_ptr<const FieldInfos> field_infos,
        const IOContext &context) const {
    const auto attribute = segment_info.attributes.find(std::string(MODE_KEY));
    if (attribute == segment_info.attributes.end()) {
        return err(LuceneError::illegal_state("missing value for " + std::string(MODE_KEY) + " for segment: " +
                                              segment_info.name));
    }
    auto mode = mode_from_name(attribute->second);
    if (!mode.has_value()) {
        return std::unexpected(std::move(mode.error()));
    }
    auto format = format_for_mode(mode.value());
    if (!format.has_value()) {
        return std::unexpected(std::move(format.error()));
    }
    auto reader = format.value().fields_reader(directory, segment_info, field_infos, context);
    if (reader.has_value() || mode.value() != Mode::BestSpeed) {
        return reader;
    }
    // A segment written by an earlier version of this port records the fast data
    // format name under the `BEST_SPEED` attribute; its chunks are plain LZ4.
    auto legacy = Lucene90CompressingStoredFieldsFormat::create(std::string(FAST_DATA_FORMAT_NAME),
                                                               CompressionMode::fast(), BEST_SPEED_CHUNK_SIZE,
                                                               BEST_SPEED_MAX_DOCS_PER_CHUNK,
                                                               BEST_SPEED_BLOCK_SHIFT);
    if (!legacy.has_value()) {
        return std::unexpected(std::move(legacy.error()));
    }
    auto legacy_reader = legacy.value().fields_reader(directory, segment_info, std::move(field_infos), context);
    if (legacy_reader.has_value()) {
        return legacy_reader;
    }
    // Neither name matched: report the error of the format the attribute asked
    // for, which is the one a caller expects to see.
    return reader;
}

Result<std::unique_ptr<StoredFieldsWriter>> Lucene90StoredFieldsFormat::fields_writer(
        Directory &directory, SegmentInfo &segment_info, const IOContext &context) const {
    const std::string key(MODE_KEY);
    const std::string value(mode_name(mode_));
    const auto existing = segment_info.attributes.find(key);
    if (existing != segment_info.attributes.end() && existing->second != value) {
        return err(LuceneError::illegal_state("found existing value for " + key + " for segment: " +
                                              segment_info.name + ", old=" + existing->second + ", new=" + value));
    }
    segment_info.attributes[key] = value;

    auto format = format_for_mode(mode_);
    if (!format.has_value()) {
        return std::unexpected(std::move(format.error()));
    }
    return format.value().fields_writer(directory, segment_info, context);
}

} // namespace lucene
