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

export module lucene_core:lucene90_compressing_term_vectors_reader;

import :types;
import std;

import :block_packed_reader_iterator;
import :close;
import :compression;
import :directory;
import :error;
import :field_infos;
import :fields;
import :index_input;
import :io_context;
import :lucene90_fields_index_reader;
import :segment_info;
import :term_vectors;
import :term_vectors_reader;

namespace lucene {

/// Reads the term vectors written by
/// [`Lucene90CompressingTermVectorsWriter`].
///
/// Port of `Lucene90CompressingTermVectorsReader`
/// (`codecs/lucene90/compressing/lucene90_compressing_term_vectors_reader.rs`).
///
/// The `.tvd` file holds compressed chunks; one chunk is decoded at a time and
/// kept in [`BlockState`], so reading the documents of a chunk in order costs
/// one decompression.  Positions, offsets and payload lengths are block packed,
/// and only the arrays the requested document actually has are decoded — the
/// streams of the other documents are skipped, which is why every packed
/// sequence is read through the same [`BlockPackedReaderIterator`] reset for the
/// new value count.
///
/// The reader keeps one mutable cursor and is therefore not thread-safe; use
/// [`clone`] to get an independent one.
export class Lucene90CompressingTermVectorsReader final : public TermVectorsReader {
public:
    [[nodiscard]] static Result<std::unique_ptr<Lucene90CompressingTermVectorsReader>> create(
            const Directory &directory, const SegmentInfo &segment_info, std::string_view segment_suffix,
            std::shared_ptr<const FieldInfos> field_infos, const IOContext &context, std::string_view format_name,
            const CompressionMode &compression_mode);

    ~Lucene90CompressingTermVectorsReader() override;

    Lucene90CompressingTermVectorsReader(const Lucene90CompressingTermVectorsReader &) = delete;
    Lucene90CompressingTermVectorsReader &operator=(const Lucene90CompressingTermVectorsReader &) = delete;

    // ---- TermVectors ----

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override;
    [[nodiscard]] Result<std::unique_ptr<Fields>> get(int32_t doc) override;

    // ---- TermVectorsReader ----

    [[nodiscard]] VoidResult check_integrity() const override;
    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> get_merge_instance() const override;
    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> clone() const override;
    [[nodiscard]] VoidResult close() const override;

    // ---- accessors ----

    [[nodiscard]] int32_t version() const noexcept { return version_; }
    [[nodiscard]] int32_t chunk_size() const noexcept { return chunk_size_; }
    [[nodiscard]] int32_t packed_ints_version() const noexcept { return packed_ints_version_; }
    [[nodiscard]] int32_t num_docs() const noexcept { return num_docs_; }
    [[nodiscard]] int64_t num_chunks() const noexcept { return num_chunks_; }
    [[nodiscard]] int64_t num_dirty_chunks() const noexcept { return num_dirty_chunks_; }
    [[nodiscard]] int64_t num_dirty_docs() const noexcept { return num_dirty_docs_; }
    [[nodiscard]] size_t max_pointer() const noexcept { return max_pointer_; }
    [[nodiscard]] const CompressionMode &compression_mode() const noexcept { return compression_mode_; }

    /// The zero-copy reader of the chunk index, used by the merge path and tests.
    [[nodiscard]] const Lucene90FieldsIndexReader &index_reader() const noexcept { return *index_reader_; }

    /// Whether the chunk currently held in memory contains `doc_id`.
    [[nodiscard]] bool is_loaded(int32_t doc_id) const noexcept;

private:
    /// The chunk that is currently decoded.
    struct BlockState {
        std::optional<size_t> start_pointer;
        std::optional<int32_t> doc_base;
        int32_t chunk_docs{0};
    };

    Lucene90CompressingTermVectorsReader(std::shared_ptr<const FieldInfos> field_infos, int32_t version,
                                         int32_t packed_ints_version, int32_t chunk_size, int32_t num_docs,
                                         std::unique_ptr<Lucene90FieldsIndexReader> index_reader,
                                         std::unique_ptr<IndexInput> vectors_stream, size_t max_pointer,
                                         int64_t num_chunks, int64_t num_dirty_chunks, int64_t num_dirty_docs,
                                         CompressionMode compression_mode);

    /// A reader over the same files with its own cursors.
    [[nodiscard]] Result<std::unique_ptr<Lucene90CompressingTermVectorsReader>> copy(
            bool for_merge) const;

    /// Cumulative end offsets of the terms of each field of the document, the
    /// input of the position/offset decoding.
    [[nodiscard]] static std::vector<std::vector<size_t>> position_index(
            size_t skip, size_t num_fields, const std::vector<size_t> &num_terms,
            const std::vector<size_t> &term_freqs);

    /// Decodes one of the three packed arrays (positions, start offsets,
    /// lengths) for the fields that have `flag` set.
    [[nodiscard]] Result<std::vector<std::vector<int32_t>>> read_positions(
            size_t skip, size_t num_fields, const std::vector<int32_t> &flags, const std::vector<size_t> &num_terms,
            const std::vector<size_t> &term_freqs, int32_t flag, size_t total_positions,
            const std::vector<std::vector<size_t>> &position_index);

    [[nodiscard]] VoidResult ensure_open() const;

    int32_t version_{0};
    int32_t packed_ints_version_{0};
    int32_t chunk_size_{0};
    int32_t num_docs_{0};
    std::shared_ptr<const FieldInfos> field_infos_;
    std::unique_ptr<Lucene90FieldsIndexReader> index_reader_;
    /// `mutable` because [`CloseableRef::close`] is `const`.
    mutable std::unique_ptr<IndexInput> vectors_stream_;
    size_t max_pointer_{0};
    int64_t num_chunks_{0};
    int64_t num_dirty_chunks_{0};
    int64_t num_dirty_docs_{0};
    CompressionMode compression_mode_;
    std::unique_ptr<Decompressor> decompressor_;
    std::unique_ptr<BlockPackedReaderIterator> reader_;
    BlockState block_state_;
    /// Ids of recently prefetched chunks, so prefetching the same block twice is
    /// free.  Small on purpose: it is scanned linearly.
    std::vector<int64_t> prefetched_block_id_cache_;
    size_t prefetched_block_id_cache_index_{0};
    mutable bool closed_{false};
};

} // namespace lucene
