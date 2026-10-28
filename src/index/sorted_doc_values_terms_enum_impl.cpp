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

module lucene_core:sorted_doc_values_terms_enum.impl;

import :sorted_doc_values_terms_enum;

import std;

import :attribute_source;
import :bytes_ref;
import :dummy_postings_enum;
import :error;
import :impacts_enum;
import :postings_enum;
import :sorted_doc_values;
import :term_state;
import :terms_enum;
import :types;

namespace lucene {

namespace {

constexpr int32_t NO_MORE_TERMS = std::numeric_limits<int32_t>::max();

} // namespace

Result<const BytesRef *> SortedDocValuesTermsEnum::load_ord(int32_t ord) const {
    auto term = values_->lookup_ord(ord);
    if (!term) {
        return std::unexpected(std::move(term.error()));
    }
    buffer_ = std::move(term.value());
    return &buffer_;
}

Result<const BytesRef *> SortedDocValuesTermsEnum::next() {
    if (values_ == nullptr) {
        return static_cast<const BytesRef *>(nullptr);
    }
    auto count = values_->get_value_count();
    if (!count) {
        return std::unexpected(std::move(count.error()));
    }
    if (current_ord_ == NO_MORE_TERMS) {
        // Stay exhausted instead of overflowing `current_ord_ + 1`.
        return static_cast<const BytesRef *>(nullptr);
    }
    current_ord_ += 1;
    if (current_ord_ >= count.value()) {
        current_ord_ = NO_MORE_TERMS;
        return static_cast<const BytesRef *>(nullptr);
    }
    return load_ord(current_ord_);
}

Result<const BytesRef *> SortedDocValuesTermsEnum::term() const { return &buffer_; }

Result<int64_t> SortedDocValuesTermsEnum::ord() const {
    if (current_ord_ == NO_MORE_TERMS) {
        return err(LuceneError::illegal_state("the enum is exhausted"));
    }
    return static_cast<int64_t>(current_ord_);
}

Result<int32_t> SortedDocValuesTermsEnum::doc_freq() {
    return err(LuceneError::unsupported_operation("doc values do not count documents per term"));
}

Result<int64_t> SortedDocValuesTermsEnum::total_term_freq() {
    return err(LuceneError::unsupported_operation("doc values do not count term frequencies"));
}

Result<std::unique_ptr<PostingsEnum>> SortedDocValuesTermsEnum::postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                                  int32_t /*flags*/) {
    return std::unique_ptr<PostingsEnum>(new DummyPostingsEnum());
}

Result<std::unique_ptr<ImpactsEnum>> SortedDocValuesTermsEnum::impacts(int32_t /*flags*/) {
    return err(LuceneError::unsupported_operation("doc values have no impacts"));
}

Result<AttributeSource *> SortedDocValuesTermsEnum::attributes() {
    return err(LuceneError::unsupported_operation("doc values have no attributes"));
}

Result<AttributeSource *> SortedDocValuesTermsEnum::attributes_mut() {
    return err(LuceneError::unsupported_operation("doc values have no attributes"));
}

Result<bool> SortedDocValuesTermsEnum::seek_exact(std::span<const uint8_t> target) {
    if (values_ == nullptr) {
        return false;
    }
    auto found = values_->lookup_term(target);
    if (!found) {
        return std::unexpected(std::move(found.error()));
    }
    if (found.value() < 0) {
        // The reference leaves `current_ord` untouched here.
        return false;
    }
    current_ord_ = found.value();
    auto loaded = load_ord(current_ord_);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    return true;
}

Result<bool> SortedDocValuesTermsEnum::prepare_seek_exact(std::span<const uint8_t> target) {
    return seek_exact(target);
}

Result<bool> SortedDocValuesTermsEnum::get_prepare_seek_exact_status(std::span<const uint8_t> target) {
    return seek_exact(target);
}

Result<SeekStatus> SortedDocValuesTermsEnum::seek_ceil(std::span<const uint8_t> target) {
    if (values_ == nullptr) {
        current_ord_ = NO_MORE_TERMS;
        return SeekStatus::End;
    }
    auto found = values_->lookup_term(target);
    if (!found) {
        return std::unexpected(std::move(found.error()));
    }
    auto count = values_->get_value_count();
    if (!count) {
        return std::unexpected(std::move(count.error()));
    }
    if (found.value() >= 0) {
        current_ord_ = found.value();
        auto loaded = load_ord(current_ord_);
        if (!loaded) {
            return std::unexpected(std::move(loaded.error()));
        }
        return SeekStatus::Found;
    }
    // A miss reports `-insertion_point - 1`, so the insertion point follows.
    current_ord_ = static_cast<int32_t>(-found.value() - 1);
    if (current_ord_ >= count.value()) {
        current_ord_ = NO_MORE_TERMS;
        return SeekStatus::End;
    }
    auto loaded = load_ord(current_ord_);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    return SeekStatus::NotFound;
}

VoidResult SortedDocValuesTermsEnum::seek_exact_with_ord(int64_t ord) {
    if (values_ == nullptr) {
        return err(LuceneError::illegal_state("the enum has no doc values"));
    }
    auto count = values_->get_value_count();
    if (!count) {
        return std::unexpected(std::move(count.error()));
    }
    if (ord < 0 || ord >= count.value()) {
        return err(LuceneError::illegal_argument(std::format("ord {} is out of range", ord)));
    }
    current_ord_ = static_cast<int32_t>(ord);
    auto loaded = load_ord(current_ord_);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    return {};
}

VoidResult SortedDocValuesTermsEnum::seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                          const TermStateEnum &state) {
    auto ord = state.ord();
    if (!ord) {
        return std::unexpected(std::move(ord.error()));
    }
    return seek_exact_with_ord(ord.value());
}

Result<TermStateEnum> SortedDocValuesTermsEnum::term_state() {
    if (current_ord_ == NO_MORE_TERMS) {
        return err(LuceneError::illegal_state("the enum is exhausted"));
    }
    return TermStateEnum(OrdTermState{static_cast<int64_t>(current_ord_)});
}

} // namespace lucene
