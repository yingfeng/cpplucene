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

module lucene_core:regexp_query.impl;

import :regexp_query;

import std;

import :automaton;
import :automaton_provider;
import :automaton_query;
import :compiled_automaton;
import :error;
import :multi_term_query;
import :operations;
import :reg_exp;
import :term;
import :types;

namespace lucene {

namespace {

constexpr int32_t kDefaultDeterminizeWorkLimit = static_cast<int32_t>(Operations::DEFAULT_DETERMINIZE_WORK_LIMIT);

/// The automaton of `regexp`, determinized when `do_determinization`.
Result<Automaton> to_automaton(const RegExp &regexp, int32_t determinize_work_limit,
                               const AutomatonProvider &provider, bool do_determinization) {
    auto automaton = regexp.to_automaton_from_provider(provider);
    if (!automaton.has_value()) {
        return std::unexpected(std::move(automaton.error()));
    }
    if (!do_determinization) {
        return std::move(automaton.value());
    }
    // `determinize` answers `nullopt` when the automaton is already
    // deterministic, in which case the automaton itself is the answer.
    auto deterministic = Operations::determinize(automaton.value(), static_cast<size_t>(determinize_work_limit));
    if (!deterministic.has_value()) {
        return std::unexpected(std::move(deterministic.error()));
    }
    if (deterministic.value().has_value()) {
        return std::move(deterministic.value().value());
    }
    return std::move(automaton.value());
}

} // namespace

RegexpQuery::RegexpQuery(Term term, CompiledAutomaton compiled, MultiTermRewriteMethod rewrite_method,
                         int32_t syntax_flags, int32_t match_flags, int32_t determinize_work_limit,
                         bool do_determinization)
        : AutomatonQuery(std::move(term), std::move(compiled), rewrite_method), syntax_flags_(syntax_flags),
          match_flags_(match_flags), determinize_work_limit_(determinize_work_limit),
          do_determinization_(do_determinization) {}

Result<RegexpQuery> RegexpQuery::create(Term term) {
    return with_flags(std::move(term), RegExp::ALL);
}

Result<RegexpQuery> RegexpQuery::with_flags(Term term, int32_t syntax_flags) {
    const DefaultProvider provider;
    return with_provider(std::move(term), syntax_flags, provider, kDefaultDeterminizeWorkLimit);
}

Result<RegexpQuery> RegexpQuery::with_flags_and_work_limit(Term term, int32_t syntax_flags,
                                                          int32_t determinize_work_limit) {
    const DefaultProvider provider;
    return with_provider(std::move(term), syntax_flags, provider, determinize_work_limit);
}

Result<RegexpQuery> RegexpQuery::with_syntax_and_match_flags(Term term, int32_t syntax_flags, int32_t match_flags,
                                                            int32_t determinize_work_limit) {
    const DefaultProvider provider;
    return with_all(std::move(term), syntax_flags, match_flags, provider, determinize_work_limit,
                    MultiTermRewriteMethod::kConstantScore);
}

Result<RegexpQuery> RegexpQuery::with_provider(Term term, int32_t syntax_flags, const AutomatonProvider &provider,
                                               int32_t determinize_work_limit) {
    return with_all(std::move(term), syntax_flags, 0, provider, determinize_work_limit,
                    MultiTermRewriteMethod::kConstantScore);
}

Result<RegexpQuery> RegexpQuery::with_all(Term term, int32_t syntax_flags, int32_t match_flags,
                                          const AutomatonProvider &provider, int32_t determinize_work_limit,
                                          MultiTermRewriteMethod rewrite_method) {
    return with_all_and_determinization(std::move(term), syntax_flags, match_flags, provider, determinize_work_limit,
                                        rewrite_method, true);
}

Result<RegexpQuery> RegexpQuery::with_all_and_determinization(Term term, int32_t syntax_flags, int32_t match_flags,
                                                             const AutomatonProvider &provider,
                                                             int32_t determinize_work_limit,
                                                             MultiTermRewriteMethod rewrite_method,
                                                             bool do_determinization) {
    auto regexp = RegExp::parse(term.text(), syntax_flags, match_flags);
    if (!regexp.has_value()) {
        return std::unexpected(std::move(regexp.error()));
    }
    auto automaton = to_automaton(regexp.value(), determinize_work_limit, provider, do_determinization);
    if (!automaton.has_value()) {
        return std::unexpected(std::move(automaton.error()));
    }
    auto compiled = compile(automaton.value(), false);
    if (!compiled.has_value()) {
        return std::unexpected(std::move(compiled.error()));
    }
    return RegexpQuery(std::move(term), std::move(compiled.value()), rewrite_method, syntax_flags, match_flags,
                       determinize_work_limit, do_determinization);
}

bool RegexpQuery::peers(const MultiTermQuery &other) const {
    const auto *regexp = dynamic_cast<const RegexpQuery *>(&other);
    if (regexp == nullptr) {
        return false;
    }
    return AutomatonQuery::peers(other) && syntax_flags_ == regexp->syntax_flags_ &&
           match_flags_ == regexp->match_flags_ && determinize_work_limit_ == regexp->determinize_work_limit_ &&
           do_determinization_ == regexp->do_determinization_;
}

Result<std::string> RegexpQuery::to_string(std::string_view field) const {
    if (!field.empty() && field == term().field()) {
        return std::format("/{}/", term().text());
    }
    return std::format("{}:/{}/", term().field(), term().text());
}

} // namespace lucene
