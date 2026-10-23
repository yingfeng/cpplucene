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

export module lucene_core:wildcard_query;

import :types;
import std;

import :automaton;
import :automaton_query;
import :compiled_automaton;
import :error;
import :multi_term_query;
import :term;
import :terms;
import :terms_enum;

namespace lucene {

/// A wildcard search: `*` matches any character sequence (including the empty
/// one), `?` matches exactly one character, and `\` escapes the next character.
///
/// Port of `WildcardQuery` (`core/search/wildcard_query.rs`).
///
/// The query can be slow, because it iterates many terms; a pattern should not
/// start with `*` for that reason.
///
/// **Deviation:** the reference implementation's `new` / `with_determinize_work_limit`
/// / `with_rewrite` constructors become [`create`] /
/// [`with_determinize_work_limit`] / [`with_rewrite`] factories returning
/// `Result`, because compiling the automaton can fail.
export class WildcardQuery final : public AutomatonQuery {
public:
    /// String equality with support for wildcards.
    static constexpr char WILDCARD_STRING = '*';
    /// Character equality with support for wildcards.
    static constexpr char WILDCARD_CHAR = '?';
    /// Escape character.
    static constexpr char WILDCARD_ESCAPE = '\\';

    /// A query for the terms matching the wildcard pattern of `term`.
    [[nodiscard]] static Result<WildcardQuery> create(Term term);

    /// [`create`], with the effort limit used to determinize the automaton.
    [[nodiscard]] static Result<WildcardQuery> with_determinize_work_limit(Term term, int32_t determinize_work_limit);

    /// [`with_determinize_work_limit`], with the rewrite method to expand the
    /// matching terms with.
    [[nodiscard]] static Result<WildcardQuery> with_rewrite(Term term, int32_t determinize_work_limit,
                                                           MultiTermRewriteMethod rewrite_method);

    /// The automaton the wildcard pattern of `term` describes, made
    /// deterministic when it is not already.
    [[nodiscard]] static Result<Automaton> to_automaton(const Term &term, int32_t determinize_work_limit);

    // ---- Query ----

    /// `field:pattern`, the form the reference implementation prints.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

protected:
    [[nodiscard]] std::string_view query_name() const override { return "WildcardQuery"; }

private:
    WildcardQuery(Term term, CompiledAutomaton compiled, MultiTermRewriteMethod rewrite_method);
};

} // namespace lucene
