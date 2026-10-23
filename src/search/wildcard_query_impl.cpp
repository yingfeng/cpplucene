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

module lucene_core:wildcard_query.impl;

import :wildcard_query;

import std;

import :automata;
import :automaton;
import :automaton_query;
import :compiled_automaton;
import :error;
import :multi_term_query;
import :operations;
import :string_helper;
import :term;
import :types;

namespace lucene {

namespace {

/// The automata of the single characters of the pattern, concatenated.
Result<Automaton> concatenate(const std::vector<Automaton> &automata) {
    std::vector<const Automaton *> parts;
    parts.reserve(automata.size());
    for (const Automaton &automaton : automata) {
        parts.push_back(&automaton);
    }
    return Operations::concatenate_list(parts);
}

} // namespace

WildcardQuery::WildcardQuery(Term term, CompiledAutomaton compiled, MultiTermRewriteMethod rewrite_method)
        : AutomatonQuery(std::move(term), std::move(compiled), rewrite_method) {}

Result<WildcardQuery> WildcardQuery::create(Term term) {
    return with_determinize_work_limit(std::move(term),
                                       static_cast<int32_t>(Operations::DEFAULT_DETERMINIZE_WORK_LIMIT));
}

Result<WildcardQuery> WildcardQuery::with_determinize_work_limit(Term term, int32_t determinize_work_limit) {
    return with_rewrite(std::move(term), determinize_work_limit, MultiTermRewriteMethod::kConstantScore);
}

Result<WildcardQuery> WildcardQuery::with_rewrite(Term term, int32_t determinize_work_limit,
                                                 MultiTermRewriteMethod rewrite_method) {
    auto automaton = to_automaton(term, determinize_work_limit);
    if (!automaton.has_value()) {
        return std::unexpected(std::move(automaton.error()));
    }
    auto compiled = compile(automaton.value(), false);
    if (!compiled.has_value()) {
        return std::unexpected(std::move(compiled.error()));
    }
    return WildcardQuery(std::move(term), std::move(compiled.value()), rewrite_method);
}

Result<Automaton> WildcardQuery::to_automaton(const Term &term, int32_t determinize_work_limit) {
    auto code_points = StringHelper::utf8_to_code_points(term.text());
    if (!code_points.has_value()) {
        return std::unexpected(std::move(code_points.error()));
    }
    const std::vector<int32_t> &pattern = code_points.value();

    std::vector<Automaton> parts;
    parts.reserve(pattern.size());
    for (size_t i = 0; i < pattern.size(); ++i) {
        Result<Automaton> part = err(LuceneError::illegal_state("unreachable"));
        switch (pattern[i]) {
            case WILDCARD_STRING:
                part = Automata::make_any_string();
                break;
            case WILDCARD_CHAR:
                part = Automata::make_any_char();
                break;
            case WILDCARD_ESCAPE: {
                // A trailing escape stands for the escape character itself,
                // like the reference implementation's `unwrap_or(c)`.
                const int32_t escaped = i + 1 < pattern.size() ? pattern[++i] : pattern[i];
                part = Automata::make_char(escaped);
                break;
            }
            default:
                part = Automata::make_char(pattern[i]);
                break;
        }
        if (!part.has_value()) {
            return std::unexpected(std::move(part.error()));
        }
        parts.push_back(std::move(part.value()));
    }

    auto concatenated = concatenate(parts);
    if (!concatenated.has_value()) {
        return std::unexpected(std::move(concatenated.error()));
    }
    // `determinize` answers `nullopt` when the automaton is already
    // deterministic, in which case the concatenation is the answer.
    auto deterministic =
            Operations::determinize(concatenated.value(), static_cast<size_t>(determinize_work_limit));
    if (!deterministic.has_value()) {
        return std::unexpected(std::move(deterministic.error()));
    }
    if (deterministic.value().has_value()) {
        return std::move(deterministic.value().value());
    }
    return std::move(concatenated.value());
}

Result<std::string> WildcardQuery::to_string(std::string_view field) const {
    if (!field.empty() && field == term().field()) {
        return term().text();
    }
    return std::format("{}:{}", term().field(), term().text());
}

} // namespace lucene
