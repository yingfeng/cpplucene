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

module lucene_core:multi_fields_impl;

import :multi_fields;

import std;

import :bytes_ref;
import :error;
import :impacts_enum;
import :postings_enum;
import :terms;
import :terms_enum;

namespace lucene {

namespace {

[[nodiscard]] VoidResult unsupported(std::string_view what) {
    return err(LuceneError::unsupported_operation(std::string(what)));
}

} // namespace

// ---------------------------------------------------------------------------
// MultiPostingsEnum
// ---------------------------------------------------------------------------

MultiPostingsEnum::MultiPostingsEnum(std::vector<Sub> subs) : subs_(std::move(subs)) {}

PostingsEnum &MultiPostingsEnum::current_postings() {
    if (!current_.has_value()) {
        throw std::logic_error("no current sub PostingsEnum");
    }
    return *subs_[current_.value()].postings;
}

const PostingsEnum &MultiPostingsEnum::current_postings() const {
    if (!current_.has_value()) {
        throw std::logic_error("no current sub PostingsEnum");
    }
    return *subs_[current_.value()].postings;
}

Result<int32_t> MultiPostingsEnum::next_doc() {
    while (true) {
        if (!current_.has_value()) {
            const size_t next = upto_.has_value() ? upto_.value() + 1 : 0;
            if (next == subs_.size()) {
                doc_ = NO_MORE_DOCS;
                return doc_;
            }
            upto_ = next;
            current_ = next;
            current_base_ = subs_[next].slice.start();
        }
        auto doc = subs_[current_.value()].postings->next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        if (doc.value() != NO_MORE_DOCS) {
            doc_ = current_base_ + doc.value();
            return doc_;
        }
        current_.reset();
    }
}

Result<int32_t> MultiPostingsEnum::advance(int32_t target) {
    while (true) {
        if (current_.has_value()) {
            auto &postings = *subs_[current_.value()].postings;
            Result<int32_t> doc = target < current_base_ ? postings.next_doc() : postings.advance(target - current_base_);
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
            if (doc.value() == NO_MORE_DOCS) {
                current_.reset();
            } else {
                doc_ = doc.value() + current_base_;
                return doc_;
            }
        } else {
            const size_t next = upto_.has_value() ? upto_.value() + 1 : 0;
            if (next == subs_.size()) {
                doc_ = NO_MORE_DOCS;
                return doc_;
            }
            upto_ = next;
            current_ = next;
            current_base_ = subs_[next].slice.start();
            if (target <= current_base_) {
                // The target falls in (or before) the slice that just started, so
                // reading its first document is enough.
                continue;
            }
        }
    }
}

Result<int64_t> MultiPostingsEnum::cost() const {
    int64_t cost = 0;
    for (const auto &sub : subs_) {
        auto sub_cost = sub.postings->cost();
        if (!sub_cost.has_value()) {
            return std::unexpected(std::move(sub_cost.error()));
        }
        cost += sub_cost.value();
    }
    return cost;
}

Result<int32_t> MultiPostingsEnum::freq() {
    return current_postings().freq();
}

Result<int32_t> MultiPostingsEnum::next_position() {
    return current_postings().next_position();
}

Result<int32_t> MultiPostingsEnum::start_offset() const {
    return current_postings().start_offset();
}

Result<int32_t> MultiPostingsEnum::end_offset() const {
    return current_postings().end_offset();
}

Result<std::optional<std::span<const uint8_t>>> MultiPostingsEnum::get_payload() const {
    return current_postings().get_payload();
}

// ---------------------------------------------------------------------------
// MultiFields
// ---------------------------------------------------------------------------

Result<std::unique_ptr<FieldNameIterator>> MultiFields::iterator() const {
    std::vector<std::string> names;
    for (const auto &sub : subs_) {
        auto inner = sub.fields->iterator();
        if (!inner.has_value()) {
            return std::unexpected(std::move(inner.error()));
        }
        while (true) {
            auto name = inner.value()->next();
            if (!name.has_value()) {
                return std::unexpected(std::move(name.error()));
            }
            if (name.value() == nullptr) {
                break;
            }
            names.push_back(*name.value());
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return std::unique_ptr<FieldNameIterator>(new MergedFieldNameIterator(std::move(names)));
}

Result<std::unique_ptr<Terms>> MultiFields::terms(std::string_view field) const {
    std::vector<MultiTerms::Sub> subs;
    subs.reserve(subs_.size());
    for (const auto &sub : subs_) {
        auto terms = sub.fields->terms(field);
        if (!terms.has_value()) {
            return std::unexpected(std::move(terms.error()));
        }
        if (terms.value() == nullptr) {
            continue;
        }
        subs.push_back(MultiTerms::Sub{std::move(terms.value()), sub.slice});
    }
    if (subs.empty()) {
        return std::unique_ptr<Terms>{};
    }
    auto multi = MultiTerms::create(std::move(subs));
    if (!multi.has_value()) {
        return std::unexpected(std::move(multi.error()));
    }
    return std::unique_ptr<Terms>(std::move(multi.value()));
}

// ---------------------------------------------------------------------------
// MultiTerms
// ---------------------------------------------------------------------------

MultiTerms::MultiTerms(std::vector<Sub> subs, bool has_freqs, bool has_offsets, bool has_positions, bool has_payloads)
        : subs_(std::move(subs)),
          has_freqs_(has_freqs),
          has_offsets_(has_offsets),
          has_positions_(has_positions),
          has_payloads_(has_payloads) {}

Result<std::unique_ptr<MultiTerms>> MultiTerms::create(std::vector<Sub> subs) {
    if (subs.empty()) {
        return err(LuceneError::illegal_argument("MultiTerms needs at least one sub"));
    }
    bool has_freqs = true;
    bool has_offsets = true;
    bool has_positions = true;
    bool has_payloads_any = false;
    for (const auto &sub : subs) {
        has_freqs = has_freqs && sub.terms->has_freqs();
        has_offsets = has_offsets && sub.terms->has_offsets();
        has_positions = has_positions && sub.terms->has_positions();
        has_payloads_any = has_payloads_any || sub.terms->has_payloads();
    }
    // Payloads are only reachable through positions.
    const bool has_payloads = has_positions && has_payloads_any;
    return std::unique_ptr<MultiTerms>(new MultiTerms(std::move(subs), has_freqs, has_offsets, has_positions,
                                                      has_payloads));
}

Result<std::unique_ptr<TermsEnum>> MultiTerms::iterator() const {
    std::vector<MultiTermsEnum::Sub> subs;
    subs.reserve(subs_.size());
    for (const auto &sub : subs_) {
        auto terms_enum = sub.terms->iterator();
        if (!terms_enum.has_value()) {
            return std::unexpected(std::move(terms_enum.error()));
        }
        subs.push_back(MultiTermsEnum::Sub{std::move(terms_enum.value()), sub.slice});
    }
    return std::unique_ptr<TermsEnum>(new MultiTermsEnum(std::move(subs)));
}

Result<int64_t> MultiTerms::sum_total_term_freq() const {
    int64_t sum = 0;
    for (const auto &sub : subs_) {
        auto value = sub.terms->sum_total_term_freq();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        if (value.value() < 0) {
            return -1;
        }
        sum += value.value();
    }
    return sum;
}

Result<int64_t> MultiTerms::sum_doc_freq() const {
    int64_t sum = 0;
    for (const auto &sub : subs_) {
        auto value = sub.terms->sum_doc_freq();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        if (value.value() < 0) {
            return -1;
        }
        sum += value.value();
    }
    return sum;
}

Result<int32_t> MultiTerms::doc_count() const {
    int32_t sum = 0;
    for (const auto &sub : subs_) {
        auto value = sub.terms->doc_count();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        if (value.value() < 0) {
            return -1;
        }
        sum += value.value();
    }
    return sum;
}

// ---------------------------------------------------------------------------
// MultiTermsEnum
// ---------------------------------------------------------------------------

Result<const BytesRef *> MultiTermsEnum::term_of(size_t index) const {
    return subs_[index].terms_enum->term();
}

Result<const BytesRef *> MultiTermsEnum::next() {
    if (!initialized_) {
        initialized_ = true;
        active_.reserve(subs_.size());
        for (size_t i = 0; i < subs_.size(); ++i) {
            auto term = subs_[i].terms_enum->next();
            if (!term.has_value()) {
                return std::unexpected(std::move(term.error()));
            }
            if (term.value() != nullptr) {
                active_.push_back(i);
            }
        }
    } else if (positioned_) {
        // Advance the subs of the previous term only now, so that they are still
        // positioned on the term returned by the previous call to `next()` while
        // the caller reads its postings (`doc_freq()`, `postings_with_flags()`).
        // The reference implementation does the same in `push_top()`.
        const std::vector<size_t> previous = std::exchange(top_, std::vector<size_t>{});
        positioned_ = false;
        for (const size_t index : previous) {
            auto term = subs_[index].terms_enum->next();
            if (!term.has_value()) {
                return std::unexpected(std::move(term.error()));
            }
            if (term.value() != nullptr) {
                active_.push_back(index);
            }
        }
    }

    // Pick the smallest term among the active subs.
    const BytesRef *smallest = nullptr;
    for (const size_t index : active_) {
        auto term = term_of(index);
        if (!term.has_value()) {
            return std::unexpected(std::move(term.error()));
        }
        if (term.value() == nullptr) {
            continue;
        }
        if (smallest == nullptr || term.value()->compare_to(smallest->as_slice()) < 0) {
            smallest = term.value();
        }
    }

    top_.clear();
    if (smallest == nullptr) {
        positioned_ = false;
        current_term_bytes_.clear();
        current_term_ = BytesRef();
        return static_cast<const BytesRef *>(nullptr);
    }

    // Own the bytes: the sub's term is only valid until its next call.
    const auto slice = smallest->as_slice();
    current_term_bytes_.assign(slice.begin(), slice.end());
    current_term_ = BytesRef(current_term_bytes_);

    std::vector<size_t> remaining;
    remaining.reserve(active_.size());
    for (const size_t index : active_) {
        auto term = term_of(index);
        if (!term.has_value()) {
            return std::unexpected(std::move(term.error()));
        }
        if (term.value() != nullptr && term.value()->compare_to(current_term_.as_slice()) == 0) {
            top_.push_back(index);
        } else {
            remaining.push_back(index);
        }
    }
    active_ = std::move(remaining);

    // The postings of the merged term are concatenated in reader order.
    std::sort(top_.begin(), top_.end());

    positioned_ = true;
    return &current_term_;
}

Result<const BytesRef *> MultiTermsEnum::term() const {
    if (!positioned_) {
        return err(LuceneError::illegal_state("MultiTermsEnum is not positioned"));
    }
    return &current_term_;
}

Result<int32_t> MultiTermsEnum::doc_freq() {
    int32_t sum = 0;
    for (const size_t index : top_) {
        auto value = subs_[index].terms_enum->doc_freq();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        sum += value.value();
    }
    return sum;
}

Result<int64_t> MultiTermsEnum::total_term_freq() {
    int64_t sum = 0;
    for (const size_t index : top_) {
        auto value = subs_[index].terms_enum->total_term_freq();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        sum += value.value();
    }
    return sum;
}

Result<std::unique_ptr<PostingsEnum>> MultiTermsEnum::postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                         int32_t flags) {
    if (!positioned_) {
        return err(LuceneError::illegal_state("MultiTermsEnum is not positioned"));
    }
    std::vector<MultiPostingsEnum::Sub> subs;
    subs.reserve(top_.size());
    for (const size_t index : top_) {
        auto postings = subs_[index].terms_enum->postings_with_flags(nullptr, flags);
        if (!postings.has_value()) {
            return std::unexpected(std::move(postings.error()));
        }
        subs.push_back(MultiPostingsEnum::Sub{std::move(postings.value()), subs_[index].slice});
    }
    return std::unique_ptr<PostingsEnum>(new MultiPostingsEnum(std::move(subs)));
}

Result<std::unique_ptr<ImpactsEnum>> MultiTermsEnum::impacts(int32_t /*flags*/) {
    return err(LuceneError::unsupported_operation("MultiTermsEnum.impacts"));
}

Result<AttributeSource *> MultiTermsEnum::attributes() {
    return err(LuceneError::unsupported_operation("MultiTermsEnum.attributes"));
}

Result<bool> MultiTermsEnum::seek_exact(std::span<const uint8_t> /*term*/) {
    return err(LuceneError::unsupported_operation("MultiTermsEnum.seek_exact"));
}

Result<bool> MultiTermsEnum::prepare_seek_exact(std::span<const uint8_t> /*term*/) {
    return err(LuceneError::unsupported_operation("MultiTermsEnum.prepare_seek_exact"));
}

Result<bool> MultiTermsEnum::get_prepare_seek_exact_status(std::span<const uint8_t> /*target*/) {
    return err(LuceneError::unsupported_operation("MultiTermsEnum.get_prepare_seek_exact_status"));
}

Result<SeekStatus> MultiTermsEnum::seek_ceil(std::span<const uint8_t> /*term*/) {
    return err(LuceneError::unsupported_operation("MultiTermsEnum.seek_ceil"));
}

VoidResult MultiTermsEnum::seek_exact_with_ord(int64_t /*ord*/) {
    return unsupported("MultiTermsEnum.seek_exact_with_ord");
}

VoidResult MultiTermsEnum::seek_exact_with_state(std::span<const uint8_t> /*term*/, const TermStateEnum & /*state*/) {
    return unsupported("MultiTermsEnum.seek_exact_with_state");
}

Result<TermStateEnum> MultiTermsEnum::term_state() {
    return err(LuceneError::unsupported_operation("MultiTermsEnum.term_state"));
}

} // namespace lucene
