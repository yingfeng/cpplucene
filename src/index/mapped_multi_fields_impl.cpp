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

module lucene_core:mapped_multi_fields.impl;

import :mapped_multi_fields;

import std;

import :doc_id_merger;
import :doc_map;
import :error;
import :merge_state;
import :multi_fields;
import :postings_enum;

namespace lucene {

namespace {

/// `IndexWriter::MAX_POSITION`: positions are stored as a vint and Lucene caps
/// them so a merge can detect a corrupt position.
constexpr int32_t kMaxPosition = (1 << 30) - 1;

} // namespace

// ---------------------------------------------------------------------------
// MappingPostingsSub
// ---------------------------------------------------------------------------

Result<int32_t> MappingPostingsSub::next_doc() {
    return postings_->next_doc();
}

// ---------------------------------------------------------------------------
// MappingMultiPostingsEnum
// ---------------------------------------------------------------------------

MappingMultiPostingsEnum::MappingMultiPostingsEnum(std::string field,
                                                   std::unique_ptr<MultiPostingsEnum> multi_postings,
                                                   DocIdMerger<MappingPostingsSub> merger)
        : field_(std::move(field)), multi_postings_(std::move(multi_postings)), merger_(std::move(merger)) {}

Result<std::unique_ptr<MappingMultiPostingsEnum>> MappingMultiPostingsEnum::create(
        std::string field, const MergeStateMeta &merge_state_meta,
        std::unique_ptr<MultiPostingsEnum> multi_postings) {
    std::vector<MappingPostingsSub> subs;
    subs.reserve(multi_postings->subs().size());
    for (const auto &sub : multi_postings->subs()) {
        const auto reader_index = sub.slice.reader_index();
        if (reader_index >= merge_state_meta.doc_maps->size()) {
            return err(LuceneError::illegal_state("postings sub-reader index out of range"));
        }
        subs.emplace_back(sub.postings.get(), merge_state_meta.doc_maps->at(reader_index).get());
    }

    std::vector<DocIdMergerSub<MappingPostingsSub>> wrapped;
    wrapped.reserve(subs.size());
    for (auto &sub : subs) {
        wrapped.emplace_back(std::move(sub));
    }

    auto merger = DocIdMerger<MappingPostingsSub>::of(std::move(wrapped), merge_state_meta.needs_index_sort);
    if (!merger.has_value()) {
        return std::unexpected(std::move(merger.error()));
    }
    return std::unique_ptr<MappingMultiPostingsEnum>(new MappingMultiPostingsEnum(
            std::move(field), std::move(multi_postings), std::move(merger.value())));
}

PostingsEnum &MappingMultiPostingsEnum::current_postings() {
    if (!current_.has_value()) {
        throw std::logic_error("no current sub PostingsEnum");
    }
    return merger_.subs()[current_.value()].sub.postings();
}

const PostingsEnum &MappingMultiPostingsEnum::current_postings() const {
    if (!current_.has_value()) {
        throw std::logic_error("no current sub PostingsEnum");
    }
    return merger_.subs()[current_.value()].sub.postings();
}

Result<int32_t> MappingMultiPostingsEnum::next_doc() {
    auto next = merger_.next();
    if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
    }
    current_ = next.value();
    if (!current_.has_value()) {
        doc_id_ = NO_MORE_DOCS;
        return doc_id_;
    }
    doc_id_ = merger_.subs()[current_.value()].mapped_doc_id;
    return doc_id_;
}

Result<int32_t> MappingMultiPostingsEnum::advance(int32_t /*target*/) {
    return err(LuceneError::unsupported_operation("advance is not supported while merging postings"));
}

Result<int64_t> MappingMultiPostingsEnum::cost() const {
    int64_t cost = 0;
    for (const auto &sub : merger_.subs()) {
        auto sub_cost = sub.sub.postings().cost();
        if (!sub_cost.has_value()) {
            return std::unexpected(std::move(sub_cost.error()));
        }
        cost += sub_cost.value();
    }
    return cost;
}

Result<int32_t> MappingMultiPostingsEnum::freq() {
    return current_postings().freq();
}

Result<int32_t> MappingMultiPostingsEnum::next_position() {
    auto position = current_postings().next_position();
    if (!position.has_value()) {
        return std::unexpected(std::move(position.error()));
    }
    if (position.value() < 0) {
        return err(LuceneError::corrupt_index(
                std::format("position={} is negative, field=\"{}\" doc={}", position.value(), field_, doc_id_)));
    }
    if (position.value() > kMaxPosition) {
        return err(LuceneError::corrupt_index(
                std::format("position={} is too large (> {}), field=\"{}\" doc={}", position.value(), kMaxPosition,
                            field_, doc_id_)));
    }
    return position;
}

Result<int32_t> MappingMultiPostingsEnum::start_offset() const {
    return current_postings().start_offset();
}

Result<int32_t> MappingMultiPostingsEnum::end_offset() const {
    return current_postings().end_offset();
}

Result<std::optional<std::span<const uint8_t>>> MappingMultiPostingsEnum::get_payload() const {
    return current_postings().get_payload();
}

// ---------------------------------------------------------------------------
// MappedMultiTerms
// ---------------------------------------------------------------------------

MappedMultiTerms::MappedMultiTerms(std::string field, std::shared_ptr<const MergeStateMeta> merge_state_meta,
                                   std::unique_ptr<Terms> inner)
        : field_(std::move(field)), merge_state_meta_(std::move(merge_state_meta)), inner_(std::move(inner)) {}

Result<std::unique_ptr<TermsEnum>> MappedMultiTerms::iterator() const {
    auto inner = inner_->iterator();
    if (!inner.has_value()) {
        return std::unexpected(std::move(inner.error()));
    }
    return std::unique_ptr<TermsEnum>(new MappedMultiTermsEnum(field_, merge_state_meta_, std::move(inner.value())));
}

Result<int64_t> MappedMultiTerms::size() const {
    return err(LuceneError::unsupported_operation("statistics are not available while merging"));
}

Result<int64_t> MappedMultiTerms::sum_total_term_freq() const {
    return err(LuceneError::unsupported_operation("statistics are not available while merging"));
}

Result<int64_t> MappedMultiTerms::sum_doc_freq() const {
    return err(LuceneError::unsupported_operation("statistics are not available while merging"));
}

Result<int32_t> MappedMultiTerms::doc_count() const {
    return err(LuceneError::unsupported_operation("statistics are not available while merging"));
}

// ---------------------------------------------------------------------------
// MappedMultiTermsEnum
// ---------------------------------------------------------------------------

MappedMultiTermsEnum::MappedMultiTermsEnum(std::string field, std::shared_ptr<const MergeStateMeta> merge_state_meta,
                                           std::unique_ptr<TermsEnum> inner)
        : field_(std::move(field)), merge_state_meta_(std::move(merge_state_meta)), inner_(std::move(inner)) {}

Result<const BytesRef *> MappedMultiTermsEnum::next() {
    return inner_->next();
}

Result<const BytesRef *> MappedMultiTermsEnum::term() const {
    return inner_->term();
}

Result<int32_t> MappedMultiTermsEnum::doc_freq() {
    return err(LuceneError::unsupported_operation("statistics are not available while merging"));
}

Result<int64_t> MappedMultiTermsEnum::total_term_freq() {
    return err(LuceneError::unsupported_operation("statistics are not available while merging"));
}

Result<std::unique_ptr<PostingsEnum>> MappedMultiTermsEnum::postings_with_flags(
        std::unique_ptr<PostingsEnum> /*reuse*/, int32_t flags) {
    auto multi = inner_->postings_with_flags(nullptr, flags);
    if (!multi.has_value()) {
        return std::unexpected(std::move(multi.error()));
    }
    auto mapping = MappingMultiPostingsEnum::create(field_, *merge_state_meta_,
                                                    std::unique_ptr<MultiPostingsEnum>(
                                                            static_cast<MultiPostingsEnum *>(multi.value().release())));
    if (!mapping.has_value()) {
        return std::unexpected(std::move(mapping.error()));
    }
    return std::unique_ptr<PostingsEnum>(std::move(mapping.value()));
}

Result<std::unique_ptr<ImpactsEnum>> MappedMultiTermsEnum::impacts(int32_t flags) {
    return inner_->impacts(flags);
}

// ---------------------------------------------------------------------------
// MappedMultiFields
// ---------------------------------------------------------------------------

MappedMultiFields::MappedMultiFields(std::shared_ptr<const MergeStateMeta> merge_state_meta,
                                     std::unique_ptr<MultiFields> inner)
        : merge_state_meta_(std::move(merge_state_meta)), inner_(std::move(inner)) {}

Result<std::unique_ptr<FieldNameIterator>> MappedMultiFields::iterator() const {
    return inner_->iterator();
}

Result<std::unique_ptr<Terms>> MappedMultiFields::terms(std::string_view field) const {
    auto terms = inner_->terms(field);
    if (!terms.has_value()) {
        return std::unexpected(std::move(terms.error()));
    }
    if (terms.value() == nullptr) {
        return std::unique_ptr<Terms>{};
    }
    return std::unique_ptr<Terms>(
            new MappedMultiTerms(std::string(field), merge_state_meta_, std::move(terms.value())));
}

} // namespace lucene
