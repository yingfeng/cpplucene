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

module lucene_core:sorting_terms.impl;

import :sorting_terms;

import std;

import :bytes_ref;
import :error;
import :field_infos;
import :fields;
import :fields_producer;
import :index_sorter;
import :index_types;
import :postings_enum;
import :sorting_postings;
import :terms;
import :terms_enum;

namespace lucene {

SortingTermsEnum::SortingTermsEnum(std::unique_ptr<TermsEnum> delegate, IndexOptions index_options,
                                  std::shared_ptr<const IndexSortDocMap> doc_map)
        : delegate_(std::move(delegate)), index_options_(index_options), doc_map_(std::move(doc_map)) {}

Result<std::unique_ptr<SortingTermsEnum>> SortingTermsEnum::create(std::unique_ptr<TermsEnum> delegate,
                                                                  IndexOptions index_options,
                                                                  std::shared_ptr<const IndexSortDocMap> doc_map) {
    if (delegate == nullptr) {
        return err(LuceneError::illegal_argument("the terms enum must not be null"));
    }
    if (doc_map == nullptr) {
        return err(LuceneError::illegal_argument("the doc map must not be null"));
    }
    return std::unique_ptr<SortingTermsEnum>(
            new SortingTermsEnum(std::move(delegate), index_options, std::move(doc_map)));
}

Result<const BytesRef *> SortingTermsEnum::next() {
    return delegate_->next();
}

Result<AttributeSource *> SortingTermsEnum::attributes() {
    return delegate_->attributes();
}

Result<AttributeSource *> SortingTermsEnum::attributes_mut() {
    return delegate_->attributes_mut();
}

Result<bool> SortingTermsEnum::seek_exact(std::span<const uint8_t> term) {
    return delegate_->seek_exact(term);
}

Result<bool> SortingTermsEnum::prepare_seek_exact(std::span<const uint8_t> term) {
    return delegate_->prepare_seek_exact(term);
}

Result<bool> SortingTermsEnum::get_prepare_seek_exact_status(std::span<const uint8_t> target) {
    return delegate_->get_prepare_seek_exact_status(target);
}

Result<SeekStatus> SortingTermsEnum::seek_ceil(std::span<const uint8_t> term) {
    return delegate_->seek_ceil(term);
}

VoidResult SortingTermsEnum::seek_exact_with_ord(int64_t ord) {
    return delegate_->seek_exact_with_ord(ord);
}

VoidResult SortingTermsEnum::seek_exact_with_state(std::span<const uint8_t> term, const TermStateEnum &state) {
    return delegate_->seek_exact_with_state(term, state);
}

Result<const BytesRef *> SortingTermsEnum::term() const {
    return delegate_->term();
}

Result<int64_t> SortingTermsEnum::ord() const {
    return delegate_->ord();
}

Result<int32_t> SortingTermsEnum::doc_freq() {
    return delegate_->doc_freq();
}

Result<int64_t> SortingTermsEnum::total_term_freq() {
    return delegate_->total_term_freq();
}

Result<std::unique_ptr<PostingsEnum>> SortingTermsEnum::postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                           int32_t flags) {
    auto inner = delegate_->postings_with_flags(nullptr, flags);
    if (!inner) {
        return std::unexpected(std::move(inner.error()));
    }

    // Frequencies can only be replayed when the field stores them; otherwise the
    // documents are renumbered on their own (the reference implementation does the
    // same split, with `SortingPostingsEnum` and `SortingDocsEnum`).
    const bool with_freqs = feature_requested(flags, FREQS);
    if (with_freqs && static_cast<int32_t>(index_options_) >= static_cast<int32_t>(IndexOptions::DocsAndFreqs)) {
        auto wrapped = SortingPostingsEnum::create(std::move(inner.value()), *doc_map_, flags);
        if (!wrapped) {
            return std::unexpected(std::move(wrapped.error()));
        }
        return std::unique_ptr<PostingsEnum>(std::move(wrapped.value()));
    }

    auto wrapped = SortingDocsEnum::create(std::move(inner.value()), *doc_map_);
    if (!wrapped) {
        return std::unexpected(std::move(wrapped.error()));
    }
    return std::unique_ptr<PostingsEnum>(std::move(wrapped.value()));
}

Result<std::unique_ptr<ImpactsEnum>> SortingTermsEnum::impacts(int32_t flags) {
    return delegate_->impacts(flags);
}

Result<TermStateEnum> SortingTermsEnum::term_state() {
    return delegate_->term_state();
}

SortingTerms::SortingTerms(std::unique_ptr<Terms> delegate, IndexOptions index_options,
                           std::shared_ptr<const IndexSortDocMap> doc_map)
        : delegate_(std::move(delegate)), index_options_(index_options), doc_map_(std::move(doc_map)) {}

Result<std::unique_ptr<SortingTerms>> SortingTerms::create(std::unique_ptr<Terms> delegate, IndexOptions index_options,
                                                          std::shared_ptr<const IndexSortDocMap> doc_map) {
    if (delegate == nullptr) {
        return err(LuceneError::illegal_argument("the terms must not be null"));
    }
    if (doc_map == nullptr) {
        return err(LuceneError::illegal_argument("the doc map must not be null"));
    }
    return std::unique_ptr<SortingTerms>(new SortingTerms(std::move(delegate), index_options, std::move(doc_map)));
}

Result<std::unique_ptr<TermsEnum>> SortingTerms::iterator() const {
    auto inner = delegate_->iterator();
    if (!inner) {
        return std::unexpected(std::move(inner.error()));
    }
    auto wrapped = SortingTermsEnum::create(std::move(inner.value()), index_options_, doc_map_);
    if (!wrapped) {
        return std::unexpected(std::move(wrapped.error()));
    }
    return std::unique_ptr<TermsEnum>(std::move(wrapped.value()));
}

Result<std::unique_ptr<TermsEnum>> SortingTerms::intersect(const CompiledAutomaton &compiled,
                                                           const BytesRef *start_term) const {
    auto inner = delegate_->intersect(compiled, start_term);
    if (!inner) {
        return std::unexpected(std::move(inner.error()));
    }
    auto wrapped = SortingTermsEnum::create(std::move(inner.value()), index_options_, doc_map_);
    if (!wrapped) {
        return std::unexpected(std::move(wrapped.error()));
    }
    return std::unique_ptr<TermsEnum>(std::move(wrapped.value()));
}

Result<int64_t> SortingTerms::size() const {
    return delegate_->size();
}

Result<int64_t> SortingTerms::sum_total_term_freq() const {
    return delegate_->sum_total_term_freq();
}

Result<int64_t> SortingTerms::sum_doc_freq() const {
    return delegate_->sum_doc_freq();
}

Result<int32_t> SortingTerms::doc_count() const {
    return delegate_->doc_count();
}

bool SortingTerms::has_freqs() const {
    return delegate_->has_freqs();
}

bool SortingTerms::has_offsets() const {
    return delegate_->has_offsets();
}

bool SortingTerms::has_positions() const {
    return delegate_->has_positions();
}

bool SortingTerms::has_payloads() const {
    return delegate_->has_payloads();
}

Result<std::optional<BytesRef>> SortingTerms::get_min() const {
    return delegate_->get_min();
}

Result<std::optional<BytesRef>> SortingTerms::get_max() const {
    return delegate_->get_max();
}

Result<std::string> SortingTerms::get_stats() const {
    return delegate_->get_stats();
}

SortingFieldsProducer::SortingFieldsProducer(std::shared_ptr<FieldsProducer> delegate,
                                             std::shared_ptr<const FieldInfos> field_infos,
                                             std::shared_ptr<const IndexSortDocMap> doc_map)
        : delegate_(std::move(delegate)), field_infos_(std::move(field_infos)), doc_map_(std::move(doc_map)) {}

Result<std::unique_ptr<SortingFieldsProducer>> SortingFieldsProducer::create(
        std::shared_ptr<FieldsProducer> delegate, std::shared_ptr<const FieldInfos> field_infos,
        std::shared_ptr<const IndexSortDocMap> doc_map) {
    if (delegate == nullptr) {
        return err(LuceneError::illegal_argument("the fields producer must not be null"));
    }
    if (field_infos == nullptr) {
        return err(LuceneError::illegal_argument("the field infos must not be null"));
    }
    if (doc_map == nullptr) {
        return err(LuceneError::illegal_argument("the doc map must not be null"));
    }
    return std::unique_ptr<SortingFieldsProducer>(
            new SortingFieldsProducer(std::move(delegate), std::move(field_infos), std::move(doc_map)));
}

Result<std::unique_ptr<FieldNameIterator>> SortingFieldsProducer::iterator() const {
    return delegate_->iterator();
}

Result<std::unique_ptr<Terms>> SortingFieldsProducer::terms(std::string_view field) const {
    auto terms = delegate_->terms(field);
    if (!terms) {
        return std::unexpected(std::move(terms.error()));
    }
    if (terms.value() == nullptr) {
        return std::unique_ptr<Terms>{};
    }

    const FieldInfo *info = field_infos_->by_name(field);
    if (info == nullptr) {
        return err(LuceneError::illegal_state("Field '" + std::string(field) + "' not found in field infos"));
    }

    auto wrapped = SortingTerms::create(std::move(terms.value()), info->index_options, doc_map_);
    if (!wrapped) {
        return std::unexpected(std::move(wrapped.error()));
    }
    return std::unique_ptr<Terms>(std::move(wrapped.value()));
}

Result<int32_t> SortingFieldsProducer::size() const {
    return delegate_->size();
}

VoidResult SortingFieldsProducer::check_integrity() const {
    return delegate_->check_integrity();
}

VoidResult SortingFieldsProducer::close() const {
    // Shared ownership: the reader that owns the producer closes it.
    return {};
}

} // namespace lucene
