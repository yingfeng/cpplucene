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

export module lucene_core:freq_prox_fields;

import :types;
import std;

import :attribute_source;
import :block_term_state;
import :byte_block_pool;
import :byte_slice_reader;
import :bytes_ref;
import :bytes_ref_block_pool;
import :bytes_ref_iterator;
import :error;
import :fields;
import :freq_prox_terms_writer_per_field;
import :impacts_enum;
import :index_types;
import :int_block_pool;
import :postings_enum;
import :terms;
import :terms_enum;

namespace lucene {

export class FreqProxDocsEnum;
export class FreqProxPostingsEnum;
export class FreqProxTerms;

/// Iterates the postings of one term of the in-memory buffer.
///
/// Port of `FreqProxTermsEnum` (`freq_prox_fields.rs`).  The terms hash must
/// have been sorted (`TermsHashPerField::sort_terms`) before the enum is used.
///
/// **Deviations** (C++ mechanics only):
///  * the term is copied into an owned `BytesRef` instead of being borrowed
///    from a pool block (`term()` needs a stable pointer either way);
///  * `attributes()` hands out an empty `AttributeSource` rather than
///    The reference implementation's `EmptyAttributeSource`, which does not exist in this port;
///  * `term_state()` returns an `OrdTermState` with the ordinal
///    (`BaseTermsEnumTermStateImpl` is not ported).
export class FreqProxTermsEnum final : public TermsEnum {
public:
    FreqProxTermsEnum(std::shared_ptr<FreqProxTermsWriterPerField> terms, std::shared_ptr<IntBlockPool> int_pool,
                      std::shared_ptr<ByteBlockPool> byte_pool);

    /// Restarts the iteration at the first term.
    void reset() noexcept { ord_ = std::nullopt; }

    // ---- BytesRefIterator ----
    [[nodiscard]] Result<const BytesRef *> next() override;

    // ---- TermsEnum ----
    [[nodiscard]] Result<AttributeSource *> attributes() override;
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override;
    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> term) override;
    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> term) override;
    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) override;
    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> text) override;
    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t ord) override;
    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> term,
                                                   const TermStateEnum &state) override;
    [[nodiscard]] Result<const BytesRef *> term() const override;
    [[nodiscard]] Result<int64_t> ord() const override;
    [[nodiscard]] Result<int32_t> doc_freq() override;
    [[nodiscard]] Result<int64_t> total_term_freq() override;
    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>>
    postings_with_flags(std::unique_ptr<PostingsEnum> reuse, int32_t flags) override;
    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t flags) override;
    [[nodiscard]] Result<TermStateEnum> term_state() override;

private:
    /// Copies the term at `position` into `current_term_`.
    void set_current_term(const BytesRefBlockPoolPosition &position);
    /// The term id of the current ordinal.
    [[nodiscard]] Result<size_t> current_term_id() const;
    [[nodiscard]] Result<BytesRefBlockPoolPosition> position_at(size_t index);
    [[nodiscard]] Result<const FreqProxPostingsArray *> postings_array() const;

    std::shared_ptr<FreqProxTermsWriterPerField> terms_;
    std::shared_ptr<IntBlockPool> int_pool_;
    std::shared_ptr<ByteBlockPool> byte_pool_;
    BytesRefBlockPool terms_pool_;
    AttributeSource attributes_;
    BytesRef current_term_;
    size_t num_terms_{0};
    std::optional<size_t> ord_;
};

/// [`Terms`] over the in-memory postings of one field.
///
/// Port of `FreqProxTerms` (`freq_prox_fields.rs`).  Statistics are not
/// available while the postings are still buffered, so `size`/`sum_*`/
/// `doc_count` report `unsupported_operation` like the reference implementation.
export class FreqProxTerms final : public Terms {
public:
    FreqProxTerms(std::shared_ptr<FreqProxTermsWriterPerField> terms, std::shared_ptr<IntBlockPool> int_pool,
                  std::shared_ptr<ByteBlockPool> byte_pool);

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override;
    [[nodiscard]] Result<int64_t> size() const override;
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override;
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override;
    [[nodiscard]] Result<int32_t> doc_count() const override;
    [[nodiscard]] bool has_freqs() const override;
    [[nodiscard]] bool has_offsets() const override;
    [[nodiscard]] bool has_positions() const override;
    [[nodiscard]] bool has_payloads() const override;

private:
    std::shared_ptr<FreqProxTermsWriterPerField> terms_;
    std::shared_ptr<IntBlockPool> int_pool_;
    std::shared_ptr<ByteBlockPool> byte_pool_;
};

/// Iterates the (already sorted) field names of a [`FreqProxFields`].
export class FreqProxFieldNameIterator final : public FieldNameIterator {
public:
    FreqProxFieldNameIterator(std::vector<std::shared_ptr<FreqProxTermsWriterPerField>> fields, size_t pos)
            : fields_(std::move(fields)), pos_(pos) {}

    [[nodiscard]] Result<const std::string *> next() override;

private:
    std::vector<std::shared_ptr<FreqProxTermsWriterPerField>> fields_;
    size_t pos_{0};
};

/// A [`Fields`] implementation over the buffered, in-memory fields.
///
/// Port of `FreqProxFields` (`freq_prox_fields.rs`): a limited view that only
/// supports iteration and `terms`, which is what `PostingsFormat` needs at flush
/// time.
export class FreqProxFields final : public Fields {
public:
    /// `fields_` must already be sorted by field name.
    FreqProxFields(std::vector<std::shared_ptr<FreqProxTermsWriterPerField>> fields,
                   std::shared_ptr<IntBlockPool> int_pool, std::shared_ptr<ByteBlockPool> byte_pool)
            : fields_(std::move(fields)), int_pool_(std::move(int_pool)), byte_pool_(std::move(byte_pool)) {}

    [[nodiscard]] Result<std::unique_ptr<FieldNameIterator>> iterator() const override;
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) const override;
    [[nodiscard]] Result<int32_t> size() const override;

private:
    std::vector<std::shared_ptr<FreqProxTermsWriterPerField>> fields_;
    std::shared_ptr<IntBlockPool> int_pool_;
    std::shared_ptr<ByteBlockPool> byte_pool_;
};

/// Docs-only postings over the in-memory buffer.
///
/// Port of `FreqProxDocsEnum` (`freq_prox_fields.rs`).
export class FreqProxDocsEnum final : public PostingsEnum {
public:
    FreqProxDocsEnum(std::shared_ptr<FreqProxTermsWriterPerField> terms, std::shared_ptr<IntBlockPool> int_pool,
                     std::shared_ptr<ByteBlockPool> byte_pool);

    /// Positions the enum on `term_id`.
    [[nodiscard]] VoidResult reset(int32_t term_id);

    [[nodiscard]] const std::shared_ptr<FreqProxTermsWriterPerField> &terms() const noexcept { return terms_; }

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    [[nodiscard]] Result<int32_t> freq() override;
    [[nodiscard]] Result<int32_t> next_position() override;
    [[nodiscard]] Result<int32_t> start_offset() const override;
    [[nodiscard]] Result<int32_t> end_offset() const override;
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override;

private:
    std::shared_ptr<FreqProxTermsWriterPerField> terms_;
    std::shared_ptr<IntBlockPool> int_pool_;
    /// Kept alive because [`reader_`] borrows it.
    std::shared_ptr<ByteBlockPool> byte_pool_;
    ByteSliceReader reader_;
    bool read_term_freq_{false};
    int32_t doc_id_{-1};
    int32_t freq_{0};
    bool ended_{false};
    std::optional<size_t> term_id_;
};

/// Postings with positions/offsets/payloads over the in-memory buffer.
///
/// Port of `FreqProxPostingsEnum` (`freq_prox_fields.rs`).
export class FreqProxPostingsEnum final : public PostingsEnum {
public:
    FreqProxPostingsEnum(std::shared_ptr<FreqProxTermsWriterPerField> terms, std::shared_ptr<IntBlockPool> int_pool,
                         std::shared_ptr<ByteBlockPool> byte_pool);

    /// Positions the enum on `term_id`.
    [[nodiscard]] VoidResult reset(int32_t term_id);

    [[nodiscard]] const std::shared_ptr<FreqProxTermsWriterPerField> &terms() const noexcept { return terms_; }

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    [[nodiscard]] Result<int32_t> freq() override;
    [[nodiscard]] Result<int32_t> next_position() override;
    [[nodiscard]] Result<int32_t> start_offset() const override;
    [[nodiscard]] Result<int32_t> end_offset() const override;
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override;

private:
    std::shared_ptr<FreqProxTermsWriterPerField> terms_;
    std::shared_ptr<IntBlockPool> int_pool_;
    /// Kept alive because the two readers below borrow it.
    std::shared_ptr<ByteBlockPool> byte_pool_;
    ByteSliceReader reader_;
    ByteSliceReader pos_reader_;
    bool read_offsets_{false};
    int32_t doc_id_{-1};
    int32_t freq_{0};
    int32_t pos_{0};
    int32_t start_offset_{0};
    int32_t end_offset_{0};
    int32_t pos_left_{0};
    size_t term_id_{0};
    bool ended_{false};
    bool has_payload_{false};
    std::vector<uint8_t> payload_;
};

} // namespace lucene
