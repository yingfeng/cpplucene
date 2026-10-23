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

module lucene_core:sorted_set_doc_values_terms_enum.impl;

import :sorted_set_doc_values_terms_enum;

import std;

import :attribute_source;
import :bytes_ref;
import :dummy_postings_enum;
import :error;
import :impacts_enum;
import :postings_enum;
import :sorted_set_doc_values;
import :term_state;
import :terms_enum;
import :types;

namespace lucene {

SortedSetDocValuesTermsEnum::SortedSetDocValuesTermsEnum(std::shared_ptr<SortedSetDocValues> values)
        : values_(std::move(values)) {}

Result<int64_t> SortedSetDocValuesTermsEnum::required_ord() const {
    if (!ord_.has_value()) {
        return err(LuceneError::illegal_state("the enum is not positioned"));
    }
    return ord_.value();
}

Result<const BytesRef *> SortedSetDocValuesTermsEnum::next() {
    if (values_ == nullptr) {
        return static_cast<const BytesRef *>(nullptr);
    }
    auto count = values_->get_value_count();
    if (!count.has_value()) {
        return std::unexpected(std::move(count.error()));
    }
    const int64_t candidate = ord_.has_value() ? ord_.value() + 1 : 0;
    if (candidate >= count.value()) {
        ord_.reset();
        return static_cast<const BytesRef *>(nullptr);
    }
    ord_ = candidate;
    auto term = values_->lookup_ord(candidate);
    if (!term.has_value()) {
        return std::unexpected(std::move(term.error()));
    }
    buffer_ = std::move(term.value());
    return &buffer_;
}

Result<const BytesRef *> SortedSetDocValuesTermsEnum::term() const {
    auto ord = required_ord();
    if (!ord.has_value()) {
        return std::unexpected(std::move(ord.error()));
    }
    if (values_ == nullptr) {
        return err(LuceneError::illegal_state("the enum has no doc values"));
    }
    auto term = values_->lookup_ord(ord.value());
    if (!term.has_value()) {
        return std::unexpected(std::move(term.error()));
    }
    buffer_ = std::move(term.value());
    return &buffer_;
}

Result<int64_t> SortedSetDocValuesTermsEnum::ord() const { return required_ord(); }

Result<std::unique_ptr<PostingsEnum>> SortedSetDocValuesTermsEnum::postings_with_flags(
        std::unique_ptr<PostingsEnum> /*reuse*/, int32_t /*flags*/) {
    return std::unique_ptr<PostingsEnum>(new DummyPostingsEnum());
}

Result<std::unique_ptr<ImpactsEnum>> SortedSetDocValuesTermsEnum::impacts(int32_t /*flags*/) {
    return err(LuceneError::unsupported_operation("doc values have no impacts"));
}

Result<AttributeSource *> SortedSetDocValuesTermsEnum::attributes() {
    return err(LuceneError::unsupported_operation("doc values have no attributes"));
}

Result<AttributeSource *> SortedSetDocValuesTermsEnum::attributes_mut() {
    return err(LuceneError::unsupported_operation("doc values have no attributes"));
}

Result<bool> SortedSetDocValuesTermsEnum::seek_exact(std::span<const uint8_t> target) {
    if (values_ == nullptr) {
        return false;
    }
    auto found = values_->lookup_term(target);
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    if (found.value() < 0) {
        ord_.reset();
        return false;
    }
    ord_ = found.value();
    return true;
}

Result<bool> SortedSetDocValuesTermsEnum::prepare_seek_exact(std::span<const uint8_t> target) {
    return seek_exact(target);
}

Result<bool> SortedSetDocValuesTermsEnum::get_prepare_seek_exact_status(std::span<const uint8_t> target) {
    return seek_exact(target);
}

Result<SeekStatus> SortedSetDocValuesTermsEnum::seek_ceil(std::span<const uint8_t> target) {
    if (values_ == nullptr) {
        ord_.reset();
        return SeekStatus::End;
    }
    auto count = values_->get_value_count();
    if (!count.has_value()) {
        return std::unexpected(std::move(count.error()));
    }
    // The dictionary is sorted by term, so a binary search over the ordinals
    // finds the smallest term that is not smaller than the target.
    int64_t low = 0;
    int64_t high = count.value();
    while (low < high) {
        const int64_t middle = low + (high - low) / 2;
        auto term = values_->lookup_ord(middle);
        if (!term.has_value()) {
            return std::unexpected(std::move(term.error()));
        }
        if (term.value().compare_to(target) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low >= count.value()) {
        ord_.reset();
        return SeekStatus::End;
    }
    ord_ = low;
    auto term = values_->lookup_ord(low);
    if (!term.has_value()) {
        return std::unexpected(std::move(term.error()));
    }
    buffer_ = std::move(term.value());
    return buffer_.compare_to(target) == 0 ? SeekStatus::Found : SeekStatus::NotFound;
}

VoidResult SortedSetDocValuesTermsEnum::seek_exact_with_ord(int64_t ord) {
    if (values_ == nullptr) {
        return err(LuceneError::illegal_state("the enum has no doc values"));
    }
    auto count = values_->get_value_count();
    if (!count.has_value()) {
        return std::unexpected(std::move(count.error()));
    }
    if (ord < 0 || ord >= count.value()) {
        return err(LuceneError::illegal_argument(std::format("ord {} is out of range", ord)));
    }
    ord_ = ord;
    return {};
}

VoidResult SortedSetDocValuesTermsEnum::seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                             const TermStateEnum &state) {
    auto ord = state.ord();
    if (!ord.has_value()) {
        return std::unexpected(std::move(ord.error()));
    }
    return seek_exact_with_ord(ord.value());
}

Result<TermStateEnum> SortedSetDocValuesTermsEnum::term_state() {
    auto ord = required_ord();
    if (!ord.has_value()) {
        return std::unexpected(std::move(ord.error()));
    }
    return TermStateEnum(OrdTermState{ord.value()});
}

} // namespace lucene
