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

export module lucene_core:block_tree_terms_writer;

import :types;
import std;

import :block_tree_terms_constants;
import :error;
import :fields;
import :fields_consumer;
import :field_infos;
import :growable_byte_array_data_output;
import :index_output;
import :norms_producer;
import :postings_writer_base;
import :segment_info;
import :segment_write_state;

namespace lucene {

/// Block-based terms index and dictionary writer.
///
/// Port of `Lucene90BlockTreeTermsWriter`
/// (`core/codecs/lucene90/block_tree/lucene90_block_tree_terms_writer.rs`).
///
/// Writes the terms dict (`.tim`), the terms index (`.tip`, an FST per field
/// mapping a term prefix to the on-disk block holding the terms with that
/// prefix) and the terms metadata (`.tmd`, one record per field).
///
/// **Deviations** (C++ mechanics only):
///  * the `O` (output) and `PW` (postings writer) type parameters become
///    `std::unique_ptr<IndexOutput>` and `std::unique_ptr<PostingsWriterBase>`:
///    type erasure replaces the generics, exactly like the reader side;
///  * `ByteBuffersDataOutput` becomes [`GrowableByteArrayDataOutput`];
///  * the reference implementation's `catch_unwind` based cleanup is not needed – C++ reports
///    failures through [`VoidResult`], so the constructor unwinds its outputs
///    explicitly on the error path;
///  * only the current format version is written (see the project's
///    "current version only" rule), so of the two `compile_index` branches only
///    the MSB vlong one survives.
///
/// The per-field machinery (`TermsWriter`, `PendingTerm`, `PendingBlock`,
/// `StatsWriter`) is internal: it only exists between [`write`] calls and is
/// therefore defined in the implementation partition.
export class BlockTreeTermsWriter final : public FieldsConsumer {
public:
    /// Lucene's `DEFAULT_MIN_BLOCK_SIZE`.
    static constexpr int32_t DEFAULT_MIN_BLOCK_SIZE = 25;
    /// Lucene's `DEFAULT_MAX_BLOCK_SIZE`.
    static constexpr int32_t DEFAULT_MAX_BLOCK_SIZE = 48;

    ~BlockTreeTermsWriter() override;

    /// Creates a writer whose blocks aim for
    /// `[min_items_in_block, max_items_in_block]` entries (blocks may be
    /// smaller than the minimum).
    ///
    /// **Deviation:** the reference implementation's constructor returns `Result<Self>`; a C++
    /// constructor cannot, so this is a factory.  It opens `.tim`, `.tip` and
    /// `.tmd`, writes their headers and initialises `postings_writer`.
    [[nodiscard]] static Result<std::unique_ptr<BlockTreeTermsWriter>>
    create(const SegmentWriteState &state, std::unique_ptr<PostingsWriterBase> postings_writer,
           int32_t min_items_in_block, int32_t max_items_in_block, const SegmentInfo &segment_info);

    /// [`create`] with [`DEFAULT_MIN_BLOCK_SIZE`] / [`DEFAULT_MAX_BLOCK_SIZE`].
    [[nodiscard]] static Result<std::unique_ptr<BlockTreeTermsWriter>>
    create_default(const SegmentWriteState &state, std::unique_ptr<PostingsWriterBase> postings_writer,
                   const SegmentInfo &segment_info);

    /// Reports an illegal-argument error when a block setting is invalid.
    [[nodiscard]] static VoidResult validate_settings(int32_t min_items_in_block, int32_t max_items_in_block);

    /// Writes every field of `fields`; see [`FieldsConsumer::write`].
    [[nodiscard]] VoidResult write(const SegmentWriteState &state, const SegmentInfo &segment_info, Fields &fields,
                                   NormsProducer *norms) override;

    /// Writes the field count and per-field metadata, then both footers and the
    /// two file lengths, and closes the four resources.  Idempotent.
    [[nodiscard]] VoidResult close() override;

    /// The block tree format version this writer emits.
    [[nodiscard]] int32_t version() const noexcept { return version_; }
    [[nodiscard]] bool is_closed() const noexcept { return closed_; }
    /// Number of fields written so far (each has one `.tmd` record).
    [[nodiscard]] size_t field_count() const noexcept { return fields_.size(); }

private:
    BlockTreeTermsWriter() = default;

    /// Releases the outputs on the error path, ignoring secondary failures
    /// (the reference implementation's `IOUtils::close_while_handling_exception`).
    void close_quietly() noexcept;

    std::unique_ptr<IndexOutput> meta_out_{};
    std::unique_ptr<IndexOutput> terms_out_{};
    std::unique_ptr<IndexOutput> index_out_{};
    int32_t max_doc_{0};
    int32_t min_items_in_block_{0};
    int32_t max_items_in_block_{0};
    int32_t version_{BLOCK_TREE_VERSION_CURRENT};
    std::unique_ptr<PostingsWriterBase> postings_writer_{};
    std::shared_ptr<const FieldInfos> field_infos_{};
    /// One `.tmd` record per field, flushed in [`close`].
    std::vector<GrowableByteArrayDataOutput> fields_{};
    bool closed_{false};
};

} // namespace lucene
