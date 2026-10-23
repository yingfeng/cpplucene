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

module lucene_core:term_in_set_query.impl;

import :term_in_set_query;

import std;

import :bytes_ref;
import :error;
import :filtered_terms_enum;
import :multi_term_query;
import :prefix_coded_terms;
import :query;
import :query_visitor;
import :term;
import :terms;
import :terms_enum;
import :types;

namespace lucene {

namespace {

/// Accepts the terms of the query, seeking through the term dictionary instead
/// of reading every term.
///
/// Port of `SetEnum` (`core/search/term_in_set_query.rs`).
class SetEnum final : public FilteredTermsEnumBase {
public:
    explicit SetEnum(const PrefixCodedTerms &term_data) : iterator_(term_data.iterator()) {
        if (auto first = advance(); !first.has_value()) {
            init_error_ = std::make_unique<LuceneError>(std::move(first.error()));
        }
    }

    [[nodiscard]] Result<AcceptStatus> accept(const BytesRef &term, int64_t /*ord*/) override {
        if (init_error_ != nullptr) {
            return std::unexpected(*init_error_);
        }
        int32_t comparison = 0;
        while (seek_term_.has_value()) {
            comparison = seek_term_.value().compare_to(term);
            if (comparison >= 0) {
                break;
            }
            if (auto advanced = advance(); !advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
        }
        if (!seek_term_.has_value()) {
            return AcceptStatus::kEnd;
        }
        return comparison == 0 ? AcceptStatus::kYesAndSeek : AcceptStatus::kNoAndSeek;
    }

    [[nodiscard]] Result<std::optional<BytesRef>> next_seek_term(const BytesRef *current) override {
        if (init_error_ != nullptr) {
            return std::unexpected(*init_error_);
        }
        if (current == nullptr) {
            return seek_term_;
        }
        while (seek_term_.has_value() && seek_term_.value().compare_to(*current) <= 0) {
            if (auto advanced = advance(); !advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
        }
        return seek_term_;
    }

private:
    /// Moves the term iterator forward and refreshes the seek term.
    [[nodiscard]] VoidResult advance() {
        auto next = iterator_.next();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        if (next.value() == nullptr) {
            seek_term_.reset();
        } else {
            seek_term_ = *next.value();
        }
        return {};
    }

    TermIterator iterator_;
    std::optional<BytesRef> seek_term_{};
    std::unique_ptr<LuceneError> init_error_{};
};

} // namespace

TermInSetQuery::TermInSetQuery(std::string field, std::shared_ptr<const PrefixCodedTerms> term_data,
                               MultiTermRewriteMethod rewrite_method)
        : MultiTermQuery(field, std::format("({} terms)", term_data == nullptr ? 0 : term_data->size()),
                         rewrite_method),
          term_data_(std::move(term_data)),
          term_data_hash_(term_data_ == nullptr ? 0 : term_data_->hash()) {}

Result<std::shared_ptr<TermInSetQuery>> TermInSetQuery::create(std::string field,
                                                              std::shared_ptr<const PrefixCodedTerms> term_data,
                                                              MultiTermRewriteMethod rewrite_method) {
    if (term_data == nullptr) {
        return err(LuceneError::illegal_argument("a term in set query needs term data"));
    }
    std::shared_ptr<TermInSetQuery> query(
            new TermInSetQuery(std::move(field), std::move(term_data), rewrite_method));
    return query;
}

int64_t TermInSetQuery::terms_count() const { return term_data_ == nullptr ? 0 : term_data_->size(); }

VoidResult TermInSetQuery::visit(QueryVisitor &visitor) const {
    if (!visitor.accept_field(field())) {
        return {};
    }
    std::vector<Term> terms;
    if (term_data_ != nullptr) {
        auto iterator = term_data_->iterator();
        while (true) {
            auto next = iterator.next();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            if (next.value() == nullptr) {
                break;
            }
            terms.emplace_back(iterator.field(), *next.value());
        }
    }
    return visitor.consume_terms(*this, terms);
}

Result<std::string> TermInSetQuery::to_string(std::string_view /*field*/) const {
    std::string builder = field();
    builder += ":(";
    if (term_data_ != nullptr) {
        auto iterator = term_data_->iterator();
        bool first = true;
        while (true) {
            auto next = iterator.next();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            if (next.value() == nullptr) {
                break;
            }
            if (!first) {
                builder.push_back(' ');
            }
            first = false;
            builder += next.value()->utf8_to_string().value();
        }
    }
    builder.push_back(')');
    return builder;
}

bool TermInSetQuery::equals(const Query &other) const {
    const auto *in_set = dynamic_cast<const TermInSetQuery *>(&other);
    if (in_set == nullptr) {
        return false;
    }
    return MultiTermQuery::equals(other);
}

size_t TermInSetQuery::hash() const {
    size_t result = MultiTermQuery::hash();
    result = result * 31 + term_data_hash_;
    return result;
}

bool TermInSetQuery::peers(const MultiTermQuery &other) const {
    const auto *in_set = dynamic_cast<const TermInSetQuery *>(&other);
    if (in_set == nullptr) {
        return false;
    }
    if (field() != in_set->field()) {
        return false;
    }
    if (term_data_ == nullptr || in_set->term_data_ == nullptr) {
        return term_data_ == in_set->term_data_;
    }
    return *term_data_ == *in_set->term_data_;
}

Result<std::unique_ptr<TermsEnum>> TermInSetQuery::terms_enum(Terms &terms) const {
    auto iterator = terms.iterator();
    if (!iterator.has_value()) {
        return std::unexpected(std::move(iterator.error()));
    }
    static const PrefixCodedTerms kEmpty;
    auto hook = std::make_unique<SetEnum>(term_data_ == nullptr ? kEmpty : *term_data_);
    std::unique_ptr<TermsEnum> filtered =
            FilteredTermsEnum::create(std::move(iterator.value()), std::move(hook), true);
    return filtered;
}

} // namespace lucene
