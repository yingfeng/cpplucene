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

export module lucene_core:term_vectors_consumer_per_field;

import :types;
import std;

import :attribute_source;
import :byte_block_pool;
import :bytes_ref;
import :bytes_ref_block_pool;
import :error;
import :field_infos;
import :field_invert_state;
import :freq_prox_terms_writer_per_field;
import :index_types;
import :indexable_field;
import :int_block_pool;
import :parallel_postings_array;
import :term_vectors_writer;
import :terms_hash_per_field;

namespace lucene {

/// Postings array of a term-vectors field.
///
/// Port of `TermVectorsPostingsArray` (`term_vectors_consumer_per_field.rs`).
/// The reference implementation keeps a [`ParallelPostingsArray`] inside (`parent`) and forwards
/// `bytes_per_posting`/`copy_to` to it; here the class derives from it instead.
export class TermVectorsPostingsArray final : public ParallelPostingsArray {
public:
    explicit TermVectorsPostingsArray(size_t size);

    [[nodiscard]] size_t bytes_per_posting() const override;
    [[nodiscard]] VoidResult copy_to(size_t new_size) override;

    /// How many times this term occurred in the current document.
    std::vector<int32_t> freqs;
    /// Last end offset seen for this term in the current document.
    std::vector<int32_t> last_offsets;
    /// Last position seen for this term in the current document.
    std::vector<int32_t> last_positions;
};

/// Builds [`TermVectorsPostingsArray`]s for [`TermsHashPerField`].
///
/// Port of `TermsHashPerFieldType::TermVectors` (`terms_hash_per_field.rs`).
export class TermVectorsTermsHashType final : public TermsHashPerFieldType {
public:
    [[nodiscard]] std::unique_ptr<PostingsArray> new_per_field(size_t size) const override;
};

/// Buffers the term vectors of one field of one document.
///
/// Port of `TermVectorsConsumerPerField`
/// (`index/term_vectors_consumer_per_field.rs`).  The class is a
/// [`TermVectorsPerField`], i.e. the link the postings terms hash chains to
/// when the field stores vectors, and it also holds the whole term-vectors
/// side of the indexer (`TermsHashPerField` over a second hash keyed by the
/// pool offset of the term text, so both hashes share the term bytes).
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation's `finish` calls back into the `TermVectorsConsumer`
///    (`add_field_to_flush`); here the consumer is the only owner of the
///    per-field writers, so the writer just raises `pending_flush()` and the
///    consumer collects it — a back pointer would buy nothing but a module
///    cycle;
///  * `write_to_writer` takes the writer as an argument instead of going
///    through the consumer's hook, because the sorting hook (index sorting) is
///    not ported.
export class TermVectorsConsumerPerField final : public TermVectorsPerField {
public:
    explicit TermVectorsConsumerPerField(std::shared_ptr<FieldInfo> field_info);

    TermVectorsConsumerPerField(const TermVectorsConsumerPerField &) = delete;
    TermVectorsConsumerPerField &operator=(const TermVectorsConsumerPerField &) = delete;
    TermVectorsConsumerPerField(TermVectorsConsumerPerField &&) noexcept = delete;
    TermVectorsConsumerPerField &operator=(TermVectorsConsumerPerField &&) noexcept = delete;
    ~TermVectorsConsumerPerField() override = default;

    [[nodiscard]] const std::shared_ptr<FieldInfo> &field_info() const noexcept { return field_info_; }

    // ---- TermVectorsPerField ----

    [[nodiscard]] bool do_vectors() const override { return do_vectors_; }
    [[nodiscard]] VoidResult start(const IndexableField &field, bool first, ByteBlockPool &byte_pool) override;
    [[nodiscard]] VoidResult add_with_text_start(int32_t text_start, int32_t doc_id,
                                                 const FieldInvertState &field_state,
                                                 const AttributeSource &attributes, IntBlockPool &int_pool,
                                                 ByteBlockPool &byte_pool, ByteBlockPool &main_byte_pool) override;
    [[nodiscard]] VoidResult finish(int32_t field_index) override;
    void reset(ByteBlockPool &byte_pool) override;

    // ---- hook for the consumer ----

    /// Whether the document that just ended left vectors to write out.
    [[nodiscard]] bool pending_flush() const noexcept { return pending_flush_; }

    /// Writes the buffered vector of the current document, if there is one.
    ///
    /// `int_pool` / `byte_pool` are the term-vectors pools (the ones
    /// [`add_with_text_start`] wrote into), `term_byte_pool` is the pool that
    /// holds the term text shared with the postings.
    [[nodiscard]] VoidResult finish_document(TermVectorsWriter &writer, const IntBlockPool &int_pool,
                                             const ByteBlockPool &byte_pool, const ByteBlockPool &term_byte_pool);

    /// Writes the buffered terms into `writer`.
    [[nodiscard]] VoidResult write_to_writer(TermVectorsWriter &writer, const IntBlockPool &int_pool,
                                             const ByteBlockPool &byte_pool, const ByteBlockPool &term_byte_pool);

private:
    [[nodiscard]] Result<int32_t> get_term_freq(const AttributeSource &attributes) const;
    [[nodiscard]] Result<TermVectorsPostingsArray *> postings();

    /// Called when a term is seen for the first time in the current document.
    [[nodiscard]] VoidResult new_term(size_t term_id, const FieldInvertState &field_state,
                                      const AttributeSource &attributes, IntBlockPool &int_pool,
                                      ByteBlockPool &byte_pool);
    /// Called when a previously seen term is seen again.
    [[nodiscard]] VoidResult add_term(size_t term_id, const FieldInvertState &field_state,
                                      const AttributeSource &attributes, IntBlockPool &int_pool,
                                      ByteBlockPool &byte_pool);
    /// Appends the position (stream 0) and offset (stream 1) deltas of a token.
    [[nodiscard]] VoidResult write_prox(size_t term_id, const FieldInvertState &field_state,
                                        const AttributeSource &attributes, IntBlockPool &int_pool,
                                        ByteBlockPool &byte_pool);

    std::shared_ptr<FieldInfo> field_info_;
    bool do_vectors_{false};
    bool do_vector_positions_{false};
    bool do_vector_offsets_{false};
    bool do_vector_payloads_{false};
    /// Set when any token of the current document carried a payload.
    bool has_payloads_{false};
    /// Set by [`finish`] once the field has vectors to write for this document.
    bool pending_flush_{false};
    /// Term text of this field, as `(offset, length)` pairs into the shared pool.
    BytesRefBlockPool term_byte_pool_;
    TermsHashPerField base_;
};

} // namespace lucene
