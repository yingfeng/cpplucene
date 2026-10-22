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

export module lucene_core:freq_prox_terms_writer_per_field;

import :types;
import std;

import :attribute_source;
import :attributes;
import :byte_block_pool;
import :bytes_ref;
import :error;
import :field_infos;
import :field_invert_state;
import :index_types;
import :indexable_field;
import :indexing_context;
import :int_block_pool;
import :parallel_postings_array;
import :terms_hash_per_field;

namespace lucene {

/// The term-vectors link of a per-field terms-hash chain.
///
/// The reference implementation's `FreqProxTermsWriterPerField::next_per_field` is a
/// `TermVectorsConsumerPerField` and calls it directly.  C++ cannot name that
/// class here without a module cycle (the term-vectors consumer builds on top of
/// this module), so the chain talks to it through this interface instead; the
/// term-vectors consumer implements it when the term-vectors write path is
/// ported.
///
/// **Deviation:** `finish` takes the field index only, not the surrounding
/// `TermVectorsConsumer`: in this port the consumer owns the per-field writers
/// (the reference implementation stores them inside the indexing chain and reads them back through
/// it), so the writer has nobody to report to and simply records that it has
/// vectors to flush.
export class TermVectorsPerField {
public:
    virtual ~TermVectorsPerField() = default;

    /// Whether term vectors must be recorded for this field.
    [[nodiscard]] virtual bool do_vectors() const = 0;

    /// Adds the current token, keyed by the postings term text offset.
    [[nodiscard]] virtual VoidResult add_with_text_start(int32_t text_start, int32_t doc_id,
                                                         const FieldInvertState &field_state,
                                                         const AttributeSource &attributes, IntBlockPool &int_pool,
                                                         ByteBlockPool &byte_pool, ByteBlockPool &main_byte_pool) = 0;

    /// Finishes the field: marks the buffered vectors for writing.
    [[nodiscard]] virtual VoidResult finish(int32_t field_index) = 0;

    /// Empties the buffers between segments.
    virtual void reset(ByteBlockPool &byte_pool) = 0;

    /// Called before a field is inverted; `field` supplies the per-document
    /// term-vector settings.
    [[nodiscard]] virtual VoidResult start(const IndexableField &field, bool first, ByteBlockPool &byte_pool) = 0;
};

/// The postings array of a `freq`/`prox` field.
///
/// Port of `FreqProxPostingsArray` (`freq_prox_terms_writer_per_field.rs`).
/// The reference implementation keeps a `ParallelPostingsArray` inside (`parent`) and forwards
/// `bytes_per_posting`/`copy_to` to it; here the class derives from it instead,
/// which is the same state with one less level of indirection.
export class FreqProxPostingsArray final : public ParallelPostingsArray {
public:
    FreqProxPostingsArray(size_t size, bool write_freqs, bool write_prox, bool write_offsets);

    [[nodiscard]] size_t bytes_per_posting() const override;
    [[nodiscard]] VoidResult copy_to(size_t new_size) override;

    /// Number of times this term occurs in the current document.
    std::optional<std::vector<int32_t>> term_freqs;
    /// Last document id where this term occurred.
    std::vector<int32_t> last_doc_ids;
    /// Code for the prior document.
    std::vector<int32_t> last_doc_codes;
    /// Last position where this term occurred.
    std::optional<std::vector<int32_t>> last_positions;
    /// Last start offset where this term occurred.
    std::optional<std::vector<int32_t>> last_offsets;
};

/// Builds [`FreqProxPostingsArray`]s for [`TermsHashPerField`].
///
/// Port of `TermsHashPerFieldType::FreqProx` (`terms_hash_per_field.rs`).
export class FreqProxTermsHashType final : public TermsHashPerFieldType {
public:
    explicit FreqProxTermsHashType(IndexOptions index_options) : index_options_(index_options) {}

    [[nodiscard]] std::unique_ptr<PostingsArray> new_per_field(size_t size) const override;

private:
    IndexOptions index_options_;
};

/// Inverts one field of one document into the postings hash.
///
/// Port of `FreqProxTermsWriterPerField`
/// (`freq_prox_terms_writer_per_field.rs`).
///
/// **Deviation:** the RAM counter is gone (no `Accountable` infrastructure), so
/// `TermsHash` no longer tracks bytes used.
export class FreqProxTermsWriterPerField {
public:
    /// `next_per_field` is borrowed, not owned: the reference implementation keeps it inside the
    /// `TermVectorsConsumer` that owns both.
    FreqProxTermsWriterPerField(std::shared_ptr<FieldInfo> field_info, TermVectorsPerField *next_per_field);

    FreqProxTermsWriterPerField(const FreqProxTermsWriterPerField &) = delete;
    FreqProxTermsWriterPerField &operator=(const FreqProxTermsWriterPerField &) = delete;
    FreqProxTermsWriterPerField(FreqProxTermsWriterPerField &&) noexcept = default;
    FreqProxTermsWriterPerField &operator=(FreqProxTermsWriterPerField &&) noexcept = default;
    ~FreqProxTermsWriterPerField() = default;

    [[nodiscard]] const std::shared_ptr<FieldInfo> &field_info() const noexcept { return field_info_; }
    [[nodiscard]] TermsHashPerField &base() noexcept { return base_; }
    [[nodiscard]] const TermsHashPerField &base() const noexcept { return base_; }
    [[nodiscard]] TermVectorsPerField *next_per_field() const noexcept { return next_per_field_; }

    [[nodiscard]] bool has_freq() const noexcept { return has_freq_; }
    [[nodiscard]] bool has_prox() const noexcept { return has_prox_; }
    [[nodiscard]] bool has_offsets() const noexcept { return has_offsets_; }
    [[nodiscard]] bool saw_payloads() const noexcept { return saw_payloads_; }

    /// Writes one position/payload code into stream 1.
    [[nodiscard]] VoidResult write_prox(size_t term_id, int32_t prox_code, const FieldInvertState &field_state,
                                        const AttributeSource &attributes, IntBlockPool &int_pool,
                                        ByteBlockPool &byte_pool);

    /// Writes the offsets of the current token into stream 1.
    [[nodiscard]] VoidResult write_offsets(size_t term_id, int32_t offset_accum, const AttributeSource &attributes,
                                           IntBlockPool &int_pool, ByteBlockPool &byte_pool);

    /// Called once per field at flush time.
    [[nodiscard]] VoidResult finish(int32_t field_index);

    /// Empties the buffers between segments.
    void reset(ByteBlockPool &byte_pool);

    /// Called before a field is inverted.
    [[nodiscard]] VoidResult start(const IndexableField &field, bool first, ByteBlockPool &byte_pool);

    /// The primary entry point: inverts one token.
    ///
    /// `term_bytes` may be null, in which case the term text is taken from the
    /// attribute source (the reference implementation's `TermToBytesRefAttribute`); the port falls
    /// back to `BytesTermAttribute`, then to `CharTermAttribute` as UTF-8.
    [[nodiscard]] VoidResult add_with_bytes_ref(const BytesRef *term_bytes, int32_t doc_id,
                                                FieldInvertState &field_state, AttributeSource &attributes,
                                                IndexContext &context);

    /// Called when a term is seen for the first time.
    [[nodiscard]] VoidResult new_term(size_t term_id, int32_t doc_id, FieldInvertState &field_state,
                                      const AttributeSource &attributes, IntBlockPool &int_pool,
                                      ByteBlockPool &byte_pool);

    /// Called when a previously seen term is seen again.
    [[nodiscard]] VoidResult add_term(size_t term_id, int32_t doc_id, FieldInvertState &field_state,
                                      const AttributeSource &attributes, IntBlockPool &int_pool,
                                      ByteBlockPool &byte_pool);

    /// Orders by field name, which is what the flush uses to sort the fields.
    [[nodiscard]] int32_t compare(const FreqProxTermsWriterPerField &other) const {
        if (field_info_->name < other.field_info_->name) {
            return -1;
        }
        return field_info_->name > other.field_info_->name ? 1 : 0;
    }

private:
    [[nodiscard]] Result<int32_t> get_term_freq(const AttributeSource &attributes) const;
    /// The postings array, checked to be a [`FreqProxPostingsArray`].
    [[nodiscard]] Result<FreqProxPostingsArray *> freq_prox_array();

    std::shared_ptr<FieldInfo> field_info_;
    /// Set when any token carried a payload in the current segment.
    bool saw_payloads_{false};
    bool has_freq_{false};
    bool has_prox_{false};
    bool has_offsets_{false};
    TermVectorsPerField *next_per_field_{nullptr};
    TermsHashPerField base_;
};

} // namespace lucene
