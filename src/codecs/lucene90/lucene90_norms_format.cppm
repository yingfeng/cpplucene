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

export module lucene_core:lucene90_norms_format;

import :types;
import :error;

import std;

import :norms_consumer;
import :norms_format;
import :norms_producer;
import :segment_info;
import :segment_read_state;
import :segment_write_state;

namespace lucene {

/// Lucene 9.0 score normalization format.
///
/// Port of `Lucene90NormsFormat`
/// (`codecs/lucene90/lucene90_norms_format.rs`).  Norms are encoded with the
/// smallest number of bytes that covers the value range of the field, which may
/// be zero when every document has the same value.
///
/// # Files
///
///  * `.nvd` — the norms data: per field, the doc-id set of the documents that
///    have a norm (an [`IndexedDISI`], omitted when every document has one)
///    followed by the values (omitted when they are all equal);
///  * `.nvm` — the metadata: per field
///    `fieldNumber, docsWithFieldAddress, docsWithFieldLength,
///    jumpTableEntryCount, denseRankPower, numDocsWithField, bytesPerNorm,
///    normsAddress`.
///
/// `docsWithFieldAddress` is `-2` when no document has a norm and `-1` when all
/// of them do; a `bytesPerNorm` of zero means every value equals
/// `normsAddress`, which then holds the value itself instead of a file pointer.
export class Lucene90NormsFormat final : public NormsFormat {
public:
    /// Codec name recorded in the `.nvd` header.
    static constexpr std::string_view DATA_CODEC = "Lucene90NormsData";
    /// Extension of the norms data file.
    static constexpr std::string_view DATA_EXTENSION = "nvd";
    /// Codec name recorded in the `.nvm` header.
    static constexpr std::string_view METADATA_CODEC = "Lucene90NormsMetadata";
    /// Extension of the norms metadata file.
    static constexpr std::string_view METADATA_EXTENSION = "nvm";

    /// First version this format ever wrote.
    static constexpr int32_t VERSION_START = 0;
    /// Version this port writes and reads (only the current one, by design).
    static constexpr int32_t VERSION_CURRENT = VERSION_START;

    [[nodiscard]] Result<std::unique_ptr<NormsConsumer>> norms_consumer(const SegmentWriteState &state,
                                                                       const SegmentInfo &segment_info) const override;

    [[nodiscard]] Result<std::unique_ptr<NormsProducer>> norms_producer(const SegmentReadState &state,
                                                                       const SegmentInfo &segment_info) const override;
};

} // namespace lucene
