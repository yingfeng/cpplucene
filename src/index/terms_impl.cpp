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

module lucene_core:terms.impl;

import :terms;

import :automaton_terms_enum;
import :bytes_ref;
import :bytes_ref_builder;
import :compiled_automaton;
import :error;
import :filtered_terms_enum;
import :single_terms_enum;
import :terms_enum;
import :types;

import std;

namespace lucene {

Result<std::optional<BytesRef>> Terms::get_min() const {
    auto iterator = this->iterator();
    if (!iterator) {
        return std::unexpected(std::move(iterator.error()));
    }
    auto term = iterator.value()->next();
    if (!term) {
        return std::unexpected(std::move(term.error()));
    }
    if (term.value() == nullptr) {
        return std::optional<BytesRef>{};
    }
    return std::optional<BytesRef>(*term.value());
}

Result<std::optional<BytesRef>> Terms::get_max() const {
    auto size_result = size();
    if (!size_result) {
        return std::unexpected(std::move(size_result.error()));
    }
    const int64_t term_count = size_result.value();
    if (term_count == 0) {
        return std::optional<BytesRef>{};
    }
    if (term_count > 0) {
        auto iterator = this->iterator();
        if (!iterator) {
            return std::unexpected(std::move(iterator.error()));
        }
        auto seeked = iterator.value()->seek_exact_with_ord(term_count - 1);
        if (seeked) {
            auto term = iterator.value()->term();
            if (!term) {
                return std::unexpected(std::move(term.error()));
            }
            return std::optional<BytesRef>(*term.value());
        }
        if (seeked.error().kind() != ErrorKind::kUnsupportedOperation) {
            return std::unexpected(std::move(seeked.error()));
        }
    }

    // The codec does not support ordinals: binary search the last term by
    // appending bytes while `seek_ceil` still finds a term.
    auto iterator = this->iterator();
    if (!iterator) {
        return std::unexpected(std::move(iterator.error()));
    }
    auto first = iterator.value()->next();
    if (!first) {
        return std::unexpected(std::move(first.error()));
    }
    if (first.value() == nullptr) {
        return std::optional<BytesRef>{};
    }

    BytesRefBuilder scratch;
    if (auto appended = scratch.append_byte(0); !appended) {
        return std::unexpected(std::move(appended.error()));
    }
    // Iterates over digits.
    while (true) {
        int32_t low = 0;
        int32_t high = 256;
        // Binary search the current digit to find the highest digit before END.
        while (low != high) {
            const int32_t mid = (low + high) >> 1;
            scratch.set_byte_at(scratch.length() - 1, static_cast<uint8_t>(mid));
            auto status = iterator.value()->seek_ceil(scratch.get_bytes_ref().as_slice());
            if (!status) {
                return std::unexpected(std::move(status.error()));
            }
            if (status.value() == SeekStatus::End) {
                if (mid == 0) {
                    scratch.set_length(scratch.length() - 1);
                    return std::optional<BytesRef>(scratch.get_bytes_owner());
                }
                high = mid;
            } else {
                if (low == mid) {
                    break;
                }
                low = mid;
            }
        }

        scratch.set_length(scratch.length() + 1);
        if (auto grown = scratch.grow(scratch.length()); !grown) {
            return std::unexpected(std::move(grown.error()));
        }
    }
}

Result<std::string> Terms::get_stats() const {
    auto term_count = size();
    if (!term_count) {
        return std::unexpected(std::move(term_count.error()));
    }
    auto doc_count_value = doc_count();
    if (!doc_count_value) {
        return std::unexpected(std::move(doc_count_value.error()));
    }
    auto sum_total = sum_total_term_freq();
    if (!sum_total) {
        return std::unexpected(std::move(sum_total.error()));
    }
    auto sum_doc = sum_doc_freq();
    if (!sum_doc) {
        return std::unexpected(std::move(sum_doc.error()));
    }
    return std::format("size={},docCount={},sumTotalTermFreq={},sumDocFreq={}", term_count.value(),
                       doc_count_value.value(), sum_total.value(), sum_doc.value());
}

Result<std::unique_ptr<TermsEnum>> Terms::intersect(const CompiledAutomaton &compiled,
                                                    const BytesRef *start_term) const {
    auto tenum = iterator();
    if (!tenum) {
        return std::unexpected(std::move(tenum.error()));
    }
    return AutomatonTermsEnum::create(std::move(tenum.value()), compiled, start_term);
}

Result<std::unique_ptr<TermsEnum>> Terms::terms_enum_for(const CompiledAutomaton &compiled) const {
    switch (compiled.type()) {
        case AutomatonType::kNone:
            return std::unique_ptr<TermsEnum>(std::make_unique<EmptyTermsEnum>());
        case AutomatonType::kAll:
            return iterator();
        case AutomatonType::kSingle: {
            if (!compiled.term().has_value()) {
                return err(LuceneError::illegal_state("term must exist for AutomatonType::kSingle"));
            }
            auto tenum = iterator();
            if (!tenum) {
                return std::unexpected(std::move(tenum.error()));
            }
            return SingleTermsEnum::create(std::move(tenum.value()), compiled.term().value());
        }
        case AutomatonType::kNormal:
            return intersect(compiled, nullptr);
    }
    return err(LuceneError::illegal_state("unknown automaton type"));
}

Result<std::unique_ptr<TermsEnum>> EmptyTerms::iterator() const {
    return std::unique_ptr<TermsEnum>(std::make_unique<EmptyTermsEnum>());
}

} // namespace lucene
