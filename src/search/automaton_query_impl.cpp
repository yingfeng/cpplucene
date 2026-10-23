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

module lucene_core:automaton_query.impl;

import :automaton_query;

import std;

import :automaton;
import :compiled_automaton;
import :error;
import :multi_term_query;
import :query;
import :term;
import :terms;
import :terms_enum;
import :types;

namespace lucene {

AutomatonQuery::AutomatonQuery(Term term, CompiledAutomaton compiled, MultiTermRewriteMethod rewrite_method)
        : MultiTermQuery(term.field(), term.text(), rewrite_method), term_(std::move(term)),
          compiled_(std::move(compiled)) {}

Result<CompiledAutomaton> AutomatonQuery::compile(const Automaton &automaton, bool is_binary) {
    // `finite = false` (unknown), `simplify = true`: the special cases (nothing,
    // everything, a single term) are cheaper to enumerate than the general form.
    return CompiledAutomaton::with_binary(automaton, false, true, is_binary);
}

Result<AutomatonQuery> AutomatonQuery::create(Term term, Automaton automaton, bool is_binary,
                                              MultiTermRewriteMethod rewrite_method) {
    auto compiled = compile(automaton, is_binary);
    if (!compiled.has_value()) {
        return std::unexpected(std::move(compiled.error()));
    }
    return AutomatonQuery(std::move(term), std::move(compiled.value()), rewrite_method);
}

Result<AutomatonQuery> AutomatonQuery::from_automaton(Term term, Automaton automaton) {
    return create(std::move(term), std::move(automaton), false, MultiTermRewriteMethod::kConstantScore);
}

Result<AutomatonQuery> AutomatonQuery::from_automaton_with_binary(Term term, Automaton automaton, bool is_binary) {
    return create(std::move(term), std::move(automaton), is_binary, MultiTermRewriteMethod::kConstantScore);
}

Result<std::unique_ptr<TermsEnum>> AutomatonQuery::terms_enum(Terms &terms) const {
    return terms.terms_enum_for(compiled_);
}

Result<std::string> AutomatonQuery::to_string(std::string_view field) const {
    if (!field.empty() && field == term_.field()) {
        return std::format("AutomatonQuery({})", term_.text());
    }
    return std::format("{}:AutomatonQuery({})", term_.field(), term_.text());
}

} // namespace lucene
