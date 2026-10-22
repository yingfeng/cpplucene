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

export module lucene_core:lucene90_compressing_stored_fields_reader;

import :types;
import std;

import :compression;
import :directory;
import :document;
import :error;
import :field_infos;
import :index_input;
import :io_context;
import :lucene90_fields_index_reader;
import :segment_info;
import :stored_field_visitor;
import :stored_fields;
import :stored_fields_reader;

namespace lucene {

/// Reads documents written by [`Lucene90CompressingStoredFieldsWriter`].
///
/// Port of `Lucene90CompressingStoredFieldsReader`
/// (`codecs/lucene90/compressing/lucene90_compressing_stored_fields_reader.rs`).
/// The reader keeps the chunk it last served decompressed, so reading the
/// documents of a chunk in order costs one decompression for the whole chunk.
///
/// **Deviation:** the reference implementation decompresses lazily, keeping only as much of the chunk
/// as the requested document needs, and streams the blocks of a sliced chunk on
/// demand.  This port decompresses the whole chunk when it is loaded instead:
/// LZ4 cannot seek, so a document in the middle of a chunk has to have
/// everything before it decoded anyway, and the chunk is bounded by `chunk_size`.
export class Lucene90CompressingStoredFieldsReader final : public StoredFieldsReader {
public:
    [[nodiscard]] static Result<std::unique_ptr<Lucene90CompressingStoredFieldsReader>> create(
            const Directory &directory, const SegmentInfo &segment_info,
            std::shared_ptr<const FieldInfos> field_infos, const IOContext &context,
            std::string_view format_name, const CompressionMode &compression_mode);

    ~Lucene90CompressingStoredFieldsReader() override;

    Lucene90CompressingStoredFieldsReader(const Lucene90CompressingStoredFieldsReader &) = delete;
    Lucene90CompressingStoredFieldsReader &operator=(const Lucene90CompressingStoredFieldsReader &) = delete;

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override;
    [[nodiscard]] VoidResult document_with_visitor(int32_t doc_id, StoredFieldVisitor &visitor) override;
    [[nodiscard]] Result<Document> document(int32_t doc_id) override;

    [[nodiscard]] VoidResult check_integrity() const override;
    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> get_merge_instance() const override;
    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> clone() const override;
    [[nodiscard]] VoidResult close() const override;

    [[nodiscard]] int32_t version() const noexcept { return version_; }
    [[nodiscard]] int32_t chunk_size() const noexcept { return chunk_size_; }
    [[nodiscard]] int32_t num_docs() const noexcept { return num_docs_; }
    [[nodiscard]] int64_t num_chunks() const noexcept { return num_chunks_; }
    [[nodiscard]] int64_t num_dirty_chunks() const noexcept { return num_dirty_chunks_; }
    [[nodiscard]] int64_t num_dirty_docs() const noexcept { return num_dirty_docs_; }
    [[nodiscard]] const CompressionMode &compression_mode() const noexcept { return compression_mode_; }

    /// The zero-copy reader of the chunk index, used by the merge path and tests.
    [[nodiscard]] const Lucene90FieldsIndexReader &index_reader() const noexcept { return *index_reader_; }

private:
    /// The state of the chunk that is currently loaded.
    struct BlockState;

    Lucene90CompressingStoredFieldsReader(int32_t version, std::shared_ptr<const FieldInfos> field_infos,
                                          std::unique_ptr<Lucene90FieldsIndexReader> index_reader,
                                          std::unique_ptr<IndexInput> fields_stream, int32_t max_pointer,
                                          int32_t chunk_size, CompressionMode compression_mode, int32_t num_docs,
                                          int64_t num_chunks, int64_t num_dirty_chunks, int64_t num_dirty_docs);

    /// Loads the chunk that holds `doc_id`; the stream must already be positioned
    /// at the chunk start.
    [[nodiscard]] VoidResult load_chunk(int32_t doc_id);

    [[nodiscard]] static VoidResult read_field(DataInput &input, StoredFieldVisitor &visitor,
                                               const FieldInfo &field_info, int32_t bits);
    [[nodiscard]] static VoidResult skip_field(DataInput &input, int32_t bits);

    int32_t version_{0};
    std::shared_ptr<const FieldInfos> field_infos_;
    std::unique_ptr<Lucene90FieldsIndexReader> index_reader_;
    /// `mutable` because [`CloseableRef::close`] is `const` and drops the handle.
    mutable std::unique_ptr<IndexInput> fields_stream_;
    int32_t max_pointer_{0};
    int32_t chunk_size_{0};
    CompressionMode compression_mode_;
    std::unique_ptr<Decompressor> decompressor_;
    int32_t num_docs_{0};
    int64_t num_chunks_{0};
    int64_t num_dirty_chunks_{0};
    int64_t num_dirty_docs_{0};
    bool merging_{false};
    std::unique_ptr<BlockState> state_;
    /// Ids of recently prefetched blocks; scanned linearly, so kept small.
    std::vector<int64_t> prefetched_block_id_cache_;
    size_t prefetched_block_id_cache_index_{0};
};

} // namespace lucene
