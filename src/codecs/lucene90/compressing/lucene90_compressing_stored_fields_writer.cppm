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

export module lucene_core:lucene90_compressing_stored_fields_writer;

import :types;
import std;

import :byte_array_output;
import :compression;
import :directory;
import :error;
import :field_infos;
import :index_output;
import :io_context;
import :lucene90_fields_index_writer;
import :segment_info;
import :stored_fields_writer;

namespace lucene {

/// Writes documents in compressed chunks.
///
/// Port of `Lucene90CompressingStoredFieldsWriter`
/// (`codecs/lucene90/compressing/lucene90_compressing_stored_fields_writer.rs`).
/// Documents are serialized back to back into an in-memory buffer; when it holds
/// `chunk_size` bytes or `max_docs_per_chunk` documents it is compressed into
/// `.fdt` and one entry is added to the `.fdx` index.  The per-document counts
/// and lengths live in the chunk header, in front of the compressed payload.
///
/// **Deviation:** `merge` is not implemented; it needs the merge state, and
/// bulk-copying chunks is only worth it once segments are merged by this codec.
export class Lucene90CompressingStoredFieldsWriter final : public StoredFieldsWriter {
public:
    [[nodiscard]] static Result<std::unique_ptr<Lucene90CompressingStoredFieldsWriter>> create(
            Directory &directory, const SegmentInfo &segment_info, const IOContext &context,
            std::string_view format_name, const CompressionMode &compression_mode, int32_t chunk_size,
            int32_t max_docs_per_chunk, int32_t block_shift);

    ~Lucene90CompressingStoredFieldsWriter() override;

    Lucene90CompressingStoredFieldsWriter(const Lucene90CompressingStoredFieldsWriter &) = delete;
    Lucene90CompressingStoredFieldsWriter &operator=(const Lucene90CompressingStoredFieldsWriter &) = delete;

    [[nodiscard]] VoidResult start_document() override;
    [[nodiscard]] VoidResult finish_document() override;

    [[nodiscard]] VoidResult write_field_i32(const FieldInfo &field_info, int32_t value) override;
    [[nodiscard]] VoidResult write_field_i64(const FieldInfo &field_info, int64_t value) override;
    [[nodiscard]] VoidResult write_field_f32(const FieldInfo &field_info, float value) override;
    [[nodiscard]] VoidResult write_field_f64(const FieldInfo &field_info, double value) override;
    [[nodiscard]] VoidResult write_field_bytes(const FieldInfo &field_info,
                                               std::span<const uint8_t> value) override;
    [[nodiscard]] VoidResult write_field_str(const FieldInfo &field_info, std::string_view value) override;

    [[nodiscard]] VoidResult finish(int32_t num_docs) override;
    [[nodiscard]] VoidResult close() override;

private:
    Lucene90CompressingStoredFieldsWriter(Directory &directory, std::string format_name,
                                          CompressionMode compression_mode, int32_t chunk_size,
                                          int32_t max_docs_per_chunk, std::unique_ptr<IndexOutput> meta_stream,
                                          std::unique_ptr<IndexOutput> fields_stream,
                                          std::unique_ptr<Lucene90FieldsIndexWriter> index_writer);

    /// Writes the chunk header: base doc id, the doc-count/flags token, then the
    /// per-document stored-field counts and byte lengths.
    [[nodiscard]] VoidResult write_header(bool sliced, bool dirty_chunk);

    /// Compresses the buffered documents into `.fdt` and resets the buffer.
    [[nodiscard]] VoidResult flush(bool force);

    [[nodiscard]] bool trigger_flush() const noexcept;

    /// `count` values of `values`, or a single vint when there is only one.
    [[nodiscard]] static VoidResult save_ints(const std::vector<int32_t> &values, size_t count, DataOutput &out);

    Directory &directory_;
    std::string format_name_;
    CompressionMode compression_mode_;
    std::unique_ptr<Compressor> compressor_;
    int32_t chunk_size_{0};
    int32_t max_docs_per_chunk_{0};
    std::unique_ptr<IndexOutput> meta_stream_;
    std::unique_ptr<IndexOutput> fields_stream_;
    std::unique_ptr<Lucene90FieldsIndexWriter> index_writer_;
    ByteArrayOutput buffered_docs_;
    std::vector<int32_t> num_stored_fields_;
    std::vector<int32_t> end_offsets_;
    size_t doc_base_{0};
    size_t num_buffered_docs_{0};
    int64_t num_chunks_{0};
    int64_t num_dirty_chunks_{0};
    int64_t num_dirty_docs_{0};
    int32_t num_stored_fields_in_doc_{0};
    bool closed_{false};
};

} // namespace lucene
