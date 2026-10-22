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

export module lucene_core:lucene90_compressing_term_vectors_writer;

import :types;
import std;

import :block_packed_writer;
import :byte_array_output;
import :bytes_ref;
import :compression;
import :data_input;
import :directory;
import :error;
import :field_infos;
import :index_output;
import :io_context;
import :lucene90_fields_index_writer;
import :segment_info;
import :term_vectors_writer;

namespace lucene {

/// Writes term vectors in compressed chunks.
///
/// Port of `Lucene90CompressingTermVectorsWriter`
/// (`codecs/lucene90/compressing/lucene90_compressing_term_vectors_writer.rs`).
///
/// Documents accumulate in memory until the buffered term suffixes reach
/// `chunk_size` bytes or `max_docs_per_chunk` documents were added; the chunk is
/// then written as
///
/// ```text
/// vint                docBase
/// vint                (chunkDocs << 1) | dirty
/// vint / block-packed number of fields of every document
///   when chunkDocs > 1, otherwise a single vint
/// byte                token: low 5 bits = bits per field number,
///                     high 3 bits = (distinct field count - 1), 7 means "more"
/// [vint]              distinct field count - 1 - 7, when the high bits are 7
/// packed              distinct field numbers
/// vlong + packed      per-field index into the distinct field numbers
/// vint + direct       flags: 0 = one flag per distinct field, 1 = one per field
/// vint + vint + packed number of terms per field
/// block-packed        prefix lengths, then suffix lengths
/// block-packed        frequencies - 1
/// block-packed        position deltas (fields with positions)
/// 4 * float           average chars per term, per distinct field (offsets)
/// block-packed        start-offset deltas (fields with offsets)
/// block-packed        lengths - term length (fields with offsets)
/// block-packed        payload lengths (fields with payloads)
/// compressed          term suffixes followed by the payload bytes
/// ```
///
/// The `dirty` flag marks a chunk that was flushed before it was full; the
/// `finish` statistics count them so a merge can decide to recompress.
///
/// **Deviation:** `merge` is not implemented, so the bulk-copy path of the reference implementation
/// (`copy_chunks` / `can_perform_bulk_merge` / `too_dirty`) does not exist here.
/// The per-document statistics it needs are still written and readable.
export class Lucene90CompressingTermVectorsWriter final : public TermVectorsWriter {
public:
    [[nodiscard]] static Result<std::unique_ptr<Lucene90CompressingTermVectorsWriter>> create(
            Directory &directory, const SegmentInfo &segment_info, std::string_view segment_suffix,
            const IOContext &context, std::string_view format_name, const CompressionMode &compression_mode,
            int32_t chunk_size, int32_t max_docs_per_chunk, int32_t block_shift);

    ~Lucene90CompressingTermVectorsWriter() override;

    Lucene90CompressingTermVectorsWriter(const Lucene90CompressingTermVectorsWriter &) = delete;
    Lucene90CompressingTermVectorsWriter &operator=(const Lucene90CompressingTermVectorsWriter &) = delete;

    [[nodiscard]] VoidResult start_document(int32_t num_vector_fields) override;
    [[nodiscard]] VoidResult finish_document() override;

    [[nodiscard]] VoidResult start_field(const FieldInfo &field_info, int32_t num_terms, bool positions,
                                         bool offsets, bool payloads) override;
    [[nodiscard]] VoidResult finish_field() override;

    [[nodiscard]] VoidResult start_term(std::span<const uint8_t> term, int32_t freq) override;
    [[nodiscard]] VoidResult add_position(int32_t position, int32_t start_offset, int32_t end_offset,
                                          std::optional<std::span<const uint8_t>> payload) override;
    [[nodiscard]] VoidResult add_prox(size_t num_prox, DataInput *positions, DataInput *offsets) override;

    [[nodiscard]] VoidResult finish(int32_t num_docs) override;
    [[nodiscard]] VoidResult close() override;

    // ---- accessors, used by tests ----
    [[nodiscard]] int32_t chunk_size() const noexcept { return chunk_size_; }
    [[nodiscard]] int64_t num_chunks() const noexcept { return num_chunks_; }
    [[nodiscard]] int64_t num_dirty_chunks() const noexcept { return num_dirty_chunks_; }
    [[nodiscard]] int64_t num_dirty_docs() const noexcept { return num_dirty_docs_; }
    [[nodiscard]] int32_t num_docs() const noexcept { return num_docs_; }

private:
    /// One pending field of a pending document.
    struct FieldData {
        bool has_positions{false};
        bool has_offsets{false};
        bool has_payloads{false};
        int32_t field_num{0};
        int32_t flags{0};
        size_t num_terms{0};
        std::vector<int32_t> freqs;
        std::vector<int32_t> prefix_lengths;
        std::vector<int32_t> suffix_lengths;
        size_t pos_start{0};
        size_t off_start{0};
        size_t pay_start{0};
        size_t total_positions{0};
        size_t ord{0};

        void add_term(int32_t freq, int32_t prefix_length, int32_t suffix_length);
        /// Stores one occurrence in the writer-owned arrays, at this field's
        /// slice of them.
        [[nodiscard]] VoidResult add_position(int32_t position, int32_t start_offset, int32_t length,
                                             int32_t payload_length, std::vector<int32_t> &positions,
                                             std::vector<int32_t> &start_offsets, std::vector<int32_t> &lengths,
                                             std::vector<int32_t> &payload_lengths);
    };

    /// One pending document.
    struct DocData {
        int32_t num_fields{0};
        std::vector<FieldData> fields;
        size_t pos_start{0};
        size_t off_start{0};
        size_t pay_start{0};

        DocData() = default;
        DocData(int32_t num_fields, size_t pos_start, size_t off_start, size_t pay_start);
        size_t add_field(int32_t field_num, size_t num_terms, bool positions, bool offsets, bool payloads);
    };

    Lucene90CompressingTermVectorsWriter(std::string format_name, CompressionMode compression_mode,
                                         int32_t chunk_size, int32_t max_docs_per_chunk,
                                         std::unique_ptr<IndexOutput> meta_stream,
                                         std::unique_ptr<IndexOutput> vectors_stream,
                                         std::unique_ptr<Lucene90FieldsIndexWriter> index_writer);

    /// Appends a document to the pending list and returns its index.
    size_t add_doc_data(int32_t num_vector_fields);

    [[nodiscard]] bool trigger_flush() const noexcept;

    /// Writes the pending chunk; `force` marks it as dirty.
    [[nodiscard]] VoidResult flush(bool force);

    [[nodiscard]] Result<int32_t> flush_num_fields(size_t chunk_docs);
    [[nodiscard]] VoidResult flush_field_nums();
    [[nodiscard]] VoidResult flush_fields(int32_t total_fields);
    [[nodiscard]] VoidResult flush_flags(int32_t total_fields);
    [[nodiscard]] VoidResult flush_num_terms(int32_t total_fields);
    [[nodiscard]] VoidResult flush_term_lengths();
    [[nodiscard]] VoidResult flush_term_freqs();
    [[nodiscard]] VoidResult flush_positions();
    [[nodiscard]] VoidResult flush_offsets();
    [[nodiscard]] VoidResult flush_payload_lengths();

    /// The field [`start_term`] and [`add_position`] append to.
    [[nodiscard]] Result<FieldData *> current_field();

    std::string format_name_;
    CompressionMode compression_mode_;
    std::unique_ptr<Compressor> compressor_;
    int32_t chunk_size_{0};
    int32_t max_docs_per_chunk_{0};
    std::unique_ptr<IndexOutput> meta_stream_;
    std::unique_ptr<IndexOutput> vectors_stream_;
    std::unique_ptr<Lucene90FieldsIndexWriter> index_writer_;

    int64_t num_chunks_{0};
    int64_t num_dirty_chunks_{0};
    int64_t num_dirty_docs_{0};
    int32_t num_docs_{0};

    std::vector<DocData> pending_docs_;
    size_t cur_doc_{0};
    size_t cur_field_{0};
    BytesRef last_term_;
    std::vector<int32_t> field_nums_;

    std::vector<int32_t> positions_buf_;
    std::vector<int32_t> start_offsets_buf_;
    std::vector<int32_t> lengths_buf_;
    std::vector<int32_t> payload_lengths_buf_;

    ByteArrayOutput term_suffixes_;
    ByteArrayOutput payload_bytes_;
    ByteArrayOutput scratch_buffer_;
    std::unique_ptr<BlockPackedWriter> writer_;
    bool closed_{false};
};

} // namespace lucene
