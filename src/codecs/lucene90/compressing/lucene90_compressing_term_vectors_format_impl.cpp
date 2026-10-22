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

module lucene_core:lucene90_compressing_term_vectors_format.impl;

import :lucene90_compressing_term_vectors_format;

import :compression;
import :directory;
import :error;
import :field_infos;
import :io_context;
import :lucene90_compressing_term_vectors_reader;
import :lucene90_compressing_term_vectors_writer;
import :segment_info;
import :term_vectors_format;
import :term_vectors_reader;
import :term_vectors_writer;

import std;

namespace lucene {

Lucene90CompressingTermVectorsFormat::Lucene90CompressingTermVectorsFormat(std::string format_name,
                                                                          std::string segment_suffix,
                                                                          CompressionMode compression_mode,
                                                                          int32_t chunk_size,
                                                                          int32_t max_docs_per_chunk,
                                                                          int32_t block_size)
    : format_name_(std::move(format_name)),
      segment_suffix_(std::move(segment_suffix)),
      compression_mode_(compression_mode),
      chunk_size_(chunk_size),
      max_docs_per_chunk_(max_docs_per_chunk),
      block_size_(block_size) {}

Result<Lucene90CompressingTermVectorsFormat> Lucene90CompressingTermVectorsFormat::create(
        std::string format_name, std::string segment_suffix, CompressionMode compression_mode, int32_t chunk_size,
        int32_t max_docs_per_chunk, int32_t block_size) {
    if (chunk_size < 1) {
        return err(LuceneError::illegal_argument("chunk_size must be >= 1"));
    }
    if (block_size < 1) {
        return err(LuceneError::illegal_argument("block_size must be >= 1"));
    }
    if (max_docs_per_chunk < 1) {
        return err(LuceneError::illegal_argument("max_docs_per_chunk must be >= 1"));
    }
    return Lucene90CompressingTermVectorsFormat(std::move(format_name), std::move(segment_suffix), compression_mode,
                                                chunk_size, max_docs_per_chunk, block_size);
}

Result<std::unique_ptr<TermVectorsReader>> Lucene90CompressingTermVectorsFormat::vectors_reader(
        const Directory &directory, const SegmentInfo &segment_info, std::shared_ptr<const FieldInfos> field_infos,
        const IOContext &context) const {
    auto reader = Lucene90CompressingTermVectorsReader::create(directory, segment_info, segment_suffix_, field_infos,
                                                              context, format_name_, compression_mode_);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    return std::unique_ptr<TermVectorsReader>(std::move(reader.value()));
}

Result<std::unique_ptr<TermVectorsWriter>> Lucene90CompressingTermVectorsFormat::vectors_writer(
        Directory &directory, const SegmentInfo &segment_info, const IOContext &context) const {
    auto writer = Lucene90CompressingTermVectorsWriter::create(directory, segment_info, segment_suffix_, context,
                                                              format_name_, compression_mode_, chunk_size_,
                                                              max_docs_per_chunk_, block_size_);
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer.error()));
    }
    return std::unique_ptr<TermVectorsWriter>(std::move(writer.value()));
}

std::string Lucene90CompressingTermVectorsFormat::to_string() const {
    return "Lucene90CompressingTermVectorsFormat(compressionMode=" + std::string(compression_mode_.name()) +
           ", chunkSize=" + std::to_string(chunk_size_) +
           ", maxDocsPerChunk=" + std::to_string(max_docs_per_chunk_) +
           ", blockSize=" + std::to_string(block_size_) + ")";
}

} // namespace lucene
