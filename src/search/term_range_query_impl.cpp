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

module lucene_core:term_range_query.impl;

import :term_range_query;

import std;

import :automata;
import :automaton;
import :automaton_query;
import :bytes_ref;
import :compiled_automaton;
import :error;
import :multi_term_query;
import :query;
import :term;
import :terms;
import :terms_enum;
import :types;

namespace lucene {

namespace {

/// The text `to_string` prints for one endpoint: `*` for an open end, and an
/// escaped `\*` for an endpoint that is literally a star.
Result<std::string> bound_text(const std::optional<BytesRef> &bound) {
    if (!bound.has_value()) {
        return std::string("*");
    }
    auto text = bound.value().utf8_to_string();
    if (!text.has_value()) {
        return std::unexpected(std::move(text.error()));
    }
    if (text.value() == "*") {
        return std::string("\\*");
    }
    return text;
}

} // namespace

TermRangeQuery::TermRangeQuery(std::optional<BytesRef> lower_term, std::optional<BytesRef> upper_term,
                               bool include_lower, bool include_upper, Term term, CompiledAutomaton compiled,
                               MultiTermRewriteMethod rewrite_method)
        : AutomatonQuery(std::move(term), std::move(compiled), rewrite_method), lower_term_(std::move(lower_term)),
          upper_term_(std::move(upper_term)), include_lower_(include_lower), include_upper_(include_upper) {}

Result<Automaton> TermRangeQuery::to_automaton(const BytesRef *lower_term, const BytesRef *upper_term,
                                               bool include_lower, bool include_upper) {
    // An open endpoint cannot be exclusive.
    const bool lower_inclusive = lower_term == nullptr ? true : include_lower;
    const bool upper_inclusive = upper_term == nullptr ? true : include_upper;
    return Automata::make_binary_interval(lower_term, lower_inclusive, upper_term, upper_inclusive);
}

Result<TermRangeQuery> TermRangeQuery::create(std::string field, std::optional<BytesRef> lower_term,
                                              std::optional<BytesRef> upper_term, bool include_lower,
                                              bool include_upper) {
    return with_rewrite(std::move(field), std::move(lower_term), std::move(upper_term), include_lower, include_upper,
                        MultiTermRewriteMethod::kConstantScore);
}

Result<TermRangeQuery> TermRangeQuery::with_rewrite(std::string field, std::optional<BytesRef> lower_term,
                                                    std::optional<BytesRef> upper_term, bool include_lower,
                                                    bool include_upper, MultiTermRewriteMethod rewrite_method) {
    auto automaton = to_automaton(lower_term.has_value() ? &lower_term.value() : nullptr,
                                  upper_term.has_value() ? &upper_term.value() : nullptr, include_lower, include_upper);
    if (!automaton.has_value()) {
        return std::unexpected(std::move(automaton.error()));
    }
    // The term only carries the field and the pattern shown by `to_string`; the
    // range automaton itself works on bytes, so it must not be converted from
    // Unicode code points.
    Term term = lower_term.has_value() ? Term(field, lower_term.value()) : Term::from_empty(field);
    auto compiled = compile(automaton.value(), true);
    if (!compiled.has_value()) {
        return std::unexpected(std::move(compiled.error()));
    }
    return TermRangeQuery(std::move(lower_term), std::move(upper_term), include_lower, include_upper, std::move(term),
                          std::move(compiled.value()), rewrite_method);
}

Result<TermRangeQuery> TermRangeQuery::create_string_range(std::string field, std::optional<std::string> lower_term,
                                                           std::optional<std::string> upper_term, bool include_lower,
                                                           bool include_upper) {
    return with_string_range_rewrite(std::move(field), std::move(lower_term), std::move(upper_term), include_lower,
                                     include_upper, MultiTermRewriteMethod::kConstantScore);
}

Result<TermRangeQuery> TermRangeQuery::with_string_range_rewrite(std::string field,
                                                                 std::optional<std::string> lower_term,
                                                                 std::optional<std::string> upper_term,
                                                                 bool include_lower, bool include_upper,
                                                                 MultiTermRewriteMethod rewrite_method) {
    std::optional<BytesRef> lower;
    if (lower_term.has_value()) {
        lower = BytesRef::from_string(lower_term.value());
    }
    std::optional<BytesRef> upper;
    if (upper_term.has_value()) {
        upper = BytesRef::from_string(upper_term.value());
    }
    return with_rewrite(std::move(field), std::move(lower), std::move(upper), include_lower, include_upper,
                        rewrite_method);
}

bool TermRangeQuery::peers(const MultiTermQuery &other) const {
    const auto *range = dynamic_cast<const TermRangeQuery *>(&other);
    if (range == nullptr) {
        return false;
    }
    if (!AutomatonQuery::peers(other) || include_lower_ != range->include_lower_ ||
        include_upper_ != range->include_upper_) {
        return false;
    }
    const auto same_bound = [](const std::optional<BytesRef> &left, const std::optional<BytesRef> &right) {
        if (left.has_value() != right.has_value()) {
            return false;
        }
        return !left.has_value() || left.value().compare_to(right.value()) == 0;
    };
    return same_bound(lower_term_, range->lower_term_) && same_bound(upper_term_, range->upper_term_);
}

Result<std::string> TermRangeQuery::to_string(std::string_view field) const {
    auto lower = bound_text(lower_term_);
    if (!lower.has_value()) {
        return std::unexpected(std::move(lower.error()));
    }
    auto upper = bound_text(upper_term_);
    if (!upper.has_value()) {
        return std::unexpected(std::move(upper.error()));
    }
    const std::string body = std::format("{}{} TO {}{}", include_lower_ ? '[' : '{', lower.value(), upper.value(),
                                         include_upper_ ? ']' : '}');
    if (!field.empty() && field == term().field()) {
        return body;
    }
    return std::format("{}:{}", term().field(), body);
}

} // namespace lucene
