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

export module lucene_core:sorting_terms;

import :types;
import std;

import :attribute_source;
import :block_term_state;
import :bytes_ref;
import :compiled_automaton;
import :error;
import :fields;
import :fields_producer;
import :field_infos;
import :impacts_enum;
import :index_sorter;
import :index_types;
import :postings_enum;
import :terms;
import :terms_enum;

namespace lucene {

/// The term dictionary of a field, presented in index-sorted order.
///
/// Port of `SortingTermsEnum` (`core/index/freq_prox_terms_writer.rs`).
///
/// The terms themselves do not move when a segment is sorted, so this wrapper
/// only forwards the enumeration and hands the index sort to the postings it
/// creates: the documents a term occurs in are renumbered through the sort,
/// while the term order, the document frequencies and the term ordinals stay
/// those of the wrapped dictionary.
///
/// **Deviations** (C++ mechanics only):
///  * `reuse` is ignored: the sorting postings materialize the whole term, so
///    there is no underlying enum left to recycle;
///  * [`impacts`] is forwarded unchanged, like the reference implementation
///    does, so an `ImpactsEnum` still reports the pre-sort document numbers.
export class SortingTermsEnum final : public TermsEnum {
public:
    /// Wraps `delegate`; the caller picks the postings wrapper from
    /// `index_options` (see [`SortingTermsEnum::create`]).
    [[nodiscard]] static Result<std::unique_ptr<SortingTermsEnum>> create(
            std::unique_ptr<TermsEnum> delegate, IndexOptions index_options,
            std::shared_ptr<const IndexSortDocMap> doc_map);

    // ---- BytesRefIterator ----

    [[nodiscard]] Result<const BytesRef *> next() override;

    // ---- TermsEnum ----

    [[nodiscard]] Result<AttributeSource *> attributes() override;
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override;
    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> term) override;
    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> term) override;
    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) override;
    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> term) override;
    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t ord) override;
    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> term,
                                                   const TermStateEnum &state) override;
    [[nodiscard]] Result<const BytesRef *> term() const override;
    [[nodiscard]] Result<int64_t> ord() const override;
    [[nodiscard]] Result<int32_t> doc_freq() override;
    [[nodiscard]] Result<int64_t> total_term_freq() override;
    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> reuse,
                                                                           int32_t flags) override;
    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t flags) override;
    [[nodiscard]] Result<TermStateEnum> term_state() override;

private:
    SortingTermsEnum(std::unique_ptr<TermsEnum> delegate, IndexOptions index_options,
                     std::shared_ptr<const IndexSortDocMap> doc_map);

    std::unique_ptr<TermsEnum> delegate_;
    IndexOptions index_options_;
    std::shared_ptr<const IndexSortDocMap> doc_map_;
};

/// The terms of one field, presented in index-sorted order.
///
/// Port of `SortingTerms` (`core/index/freq_prox_terms_writer.rs`).
///
/// Everything except [`iterator`] and [`intersect`] is forwarded: the number of
/// terms, the summed frequencies and the feature flags describe the dictionary,
/// which the sort does not change.
///
/// **Deviation:** `index_options` comes from the caller (the wrapping `Fields`
/// looks it up in the field infos) instead of being read from a `FieldInfo`,
/// because the port's `Terms` has no back pointer to one.
export class SortingTerms final : public Terms {
public:
    /// Wraps `delegate`; `index_options` selects the postings wrapper used by
    /// the enums this instance creates.
    [[nodiscard]] static Result<std::unique_ptr<SortingTerms>> create(
            std::unique_ptr<Terms> delegate, IndexOptions index_options,
            std::shared_ptr<const IndexSortDocMap> doc_map);

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override;
    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> intersect(const CompiledAutomaton &compiled,
                                                              const BytesRef *start_term) const override;

    [[nodiscard]] Result<int64_t> size() const override;
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override;
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override;
    [[nodiscard]] Result<int32_t> doc_count() const override;
    [[nodiscard]] bool has_freqs() const override;
    [[nodiscard]] bool has_offsets() const override;
    [[nodiscard]] bool has_positions() const override;
    [[nodiscard]] bool has_payloads() const override;
    [[nodiscard]] Result<std::optional<BytesRef>> get_min() const override;
    [[nodiscard]] Result<std::optional<BytesRef>> get_max() const override;
    [[nodiscard]] Result<std::string> get_stats() const override;

private:
    SortingTerms(std::unique_ptr<Terms> delegate, IndexOptions index_options,
                 std::shared_ptr<const IndexSortDocMap> doc_map);

    std::unique_ptr<Terms> delegate_;
    IndexOptions index_options_;
    std::shared_ptr<const IndexSortDocMap> doc_map_;
};

/// The postings of a segment, presented in index-sorted order.
///
/// Port of `FilterFieldsImpl` and `FieldsProducerImpl`
/// (`core/index/freq_prox_terms_writer.rs`, `core/index/sorting_codec_reader.rs`).
///
/// [`terms`] returns a [`SortingTerms`]; the per-field index options needed to
/// choose the postings wrapper come from the segment's field infos.
///
/// **Deviation:** [`close`] does nothing: the port shares the wrapped producer
/// between readers through a `std::shared_ptr`, so the owner closes it.
export class SortingFieldsProducer final : public FieldsProducer {
public:
    /// Wraps `delegate`; all three arguments must outlive the wrapper (or, for
    /// the producer, be shared with it).
    [[nodiscard]] static Result<std::unique_ptr<SortingFieldsProducer>> create(
            std::shared_ptr<FieldsProducer> delegate, std::shared_ptr<const FieldInfos> field_infos,
            std::shared_ptr<const IndexSortDocMap> doc_map);

    [[nodiscard]] Result<std::unique_ptr<FieldNameIterator>> iterator() const override;
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) const override;
    [[nodiscard]] Result<int32_t> size() const override;

    [[nodiscard]] VoidResult check_integrity() const override;
    [[nodiscard]] VoidResult close() const override;

private:
    SortingFieldsProducer(std::shared_ptr<FieldsProducer> delegate, std::shared_ptr<const FieldInfos> field_infos,
                          std::shared_ptr<const IndexSortDocMap> doc_map);

    std::shared_ptr<FieldsProducer> delegate_;
    std::shared_ptr<const FieldInfos> field_infos_;
    std::shared_ptr<const IndexSortDocMap> doc_map_;
};

} // namespace lucene
