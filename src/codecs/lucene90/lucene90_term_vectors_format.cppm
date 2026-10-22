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

export module lucene_core:lucene90_term_vectors_format;

import :types;
import std;

import :error;
import :field_infos;
import :io_context;
import :lucene90_compressing_term_vectors_format;
import :segment_info;
import :term_vectors_format;
import :term_vectors_reader;
import :term_vectors_writer;

namespace lucene {

/// Lucene 9.0 term vectors format.
///
/// Port of `Lucene90TermVectorsFormat`
/// (`core/codecs/lucene90/lucene90_term_vectors_format.rs`): a one-line wrapper
/// around [`Lucene90CompressingTermVectorsFormat`] with Lucene's default
/// parameters (LZ4 fast, 4 KB chunks, 128 documents per chunk, 10 chunks per
/// index block).
export class Lucene90TermVectorsFormat final : public TermVectorsFormat {
public:
    /// Format name recorded in the `.tvd` header.
    static constexpr std::string_view FORMAT_NAME = "Lucene90TermVectors";
    /// Name of the data format the wrapper hides.
    static constexpr std::string_view DATA_FORMAT_NAME = "Lucene90TermVectorsData";

    /// Lucene's default chunk size: 4 KB.
    static constexpr int32_t DEFAULT_CHUNK_SIZE = 1 << 12;
    /// Lucene's default maximum number of documents per chunk.
    static constexpr int32_t DEFAULT_MAX_DOCS_PER_CHUNK = 128;
    /// Number of chunks per index block.
    static constexpr int32_t DEFAULT_BLOCK_SIZE = 10;

    /// Creates the format, with Lucene's default parameters.
    [[nodiscard]] static Result<Lucene90TermVectorsFormat> create() {
        auto base = Lucene90CompressingTermVectorsFormat::create(std::string(DATA_FORMAT_NAME), "",
                                                                 CompressionMode::fast(), DEFAULT_CHUNK_SIZE,
                                                                 DEFAULT_MAX_DOCS_PER_CHUNK, DEFAULT_BLOCK_SIZE);
        if (!base.has_value()) {
            return std::unexpected(std::move(base.error()));
        }
        return Lucene90TermVectorsFormat(std::move(base.value()));
    }

    [[nodiscard]] std::string_view name() const override { return FORMAT_NAME; }

    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> vectors_reader(
            const Directory &directory, const SegmentInfo &segment_info,
            std::shared_ptr<const FieldInfos> field_infos, const IOContext &context) const override {
        return base_.vectors_reader(directory, segment_info, std::move(field_infos), context);
    }

    [[nodiscard]] Result<std::unique_ptr<TermVectorsWriter>> vectors_writer(
            Directory &directory, const SegmentInfo &segment_info, const IOContext &context) const override {
        return base_.vectors_writer(directory, segment_info, context);
    }

    /// The wrapped compressing format, for diagnostics and tests.
    [[nodiscard]] const Lucene90CompressingTermVectorsFormat &base() const noexcept { return base_; }

private:
    explicit Lucene90TermVectorsFormat(Lucene90CompressingTermVectorsFormat base) : base_(std::move(base)) {}

    Lucene90CompressingTermVectorsFormat base_;
};

} // namespace lucene
