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

export module lucene_core:regexp_query;

import :types;
import std;

import :automaton;
import :automaton_provider;
import :automaton_query;
import :compiled_automaton;
import :error;
import :multi_term_query;
import :term;
import :terms;
import :terms_enum;

namespace lucene {

/// A regular expression query over the [`RegExp`](:reg_exp) syntax; the term
/// dictionary is enumerated with an automaton instead of compared term by term.
///
/// Port of `RegexpQuery` (`core/search/regexp_query.rs`).  Note that the syntax
/// is the one of [`RegExp`](:reg_exp), which differs from other regular
/// expression implementations, and that a pattern should not start with `.*`
/// for performance.
///
/// **Deviation:** the reference implementation's constructors become static
/// factories returning `Result`, because compiling the automaton can fail.
export class RegexpQuery final : public AutomatonQuery {
public:
    /// A query for the terms matching the regular expression of `term`, with all
    /// syntax features enabled.
    [[nodiscard]] static Result<RegexpQuery> create(Term term);

    /// [`create`], with the [`RegExp`](:reg_exp) syntax flags to enable.
    [[nodiscard]] static Result<RegexpQuery> with_flags(Term term, int32_t syntax_flags);

    /// [`with_flags`], with the effort limit used to determinize the automaton.
    [[nodiscard]] static Result<RegexpQuery> with_flags_and_work_limit(Term term, int32_t syntax_flags,
                                                                      int32_t determinize_work_limit);

    /// [`with_flags_and_work_limit`], with the match behavior flags (for example
    /// case insensitivity).
    [[nodiscard]] static Result<RegexpQuery> with_syntax_and_match_flags(Term term, int32_t syntax_flags,
                                                                        int32_t match_flags,
                                                                        int32_t determinize_work_limit);

    /// [`with_flags_and_work_limit`], resolving named automata through
    /// `provider`.
    [[nodiscard]] static Result<RegexpQuery> with_provider(Term term, int32_t syntax_flags,
                                                          const AutomatonProvider &provider,
                                                          int32_t determinize_work_limit);

    /// [`with_syntax_and_match_flags`], with the provider for named automata and
    /// the rewrite method.
    [[nodiscard]] static Result<RegexpQuery> with_all(Term term, int32_t syntax_flags, int32_t match_flags,
                                                     const AutomatonProvider &provider, int32_t determinize_work_limit,
                                                     MultiTermRewriteMethod rewrite_method);

    /// [`with_all`], with `do_determinization` telling whether the automaton must
    /// be determinized.  Without determinization an NFA may remain, which is not
    /// thread safe, so an executor backed searcher should keep determinization on.
    [[nodiscard]] static Result<RegexpQuery> with_all_and_determinization(Term term, int32_t syntax_flags,
                                                                         int32_t match_flags,
                                                                         const AutomatonProvider &provider,
                                                                         int32_t determinize_work_limit,
                                                                         MultiTermRewriteMethod rewrite_method,
                                                                         bool do_determinization);

    [[nodiscard]] int32_t syntax_flags() const noexcept { return syntax_flags_; }
    [[nodiscard]] int32_t match_flags() const noexcept { return match_flags_; }
    [[nodiscard]] int32_t determinize_work_limit() const noexcept { return determinize_work_limit_; }
    [[nodiscard]] bool do_determinization() const noexcept { return do_determinization_; }

    // ---- Query ----

    /// `field:/pattern/`, the form the reference implementation prints.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

protected:
    [[nodiscard]] std::string_view query_name() const override { return "RegexpQuery"; }

    [[nodiscard]] bool peers(const MultiTermQuery &other) const override;

private:
    RegexpQuery(Term term, CompiledAutomaton compiled, MultiTermRewriteMethod rewrite_method, int32_t syntax_flags,
                int32_t match_flags, int32_t determinize_work_limit, bool do_determinization);

    int32_t syntax_flags_{0};
    int32_t match_flags_{0};
    int32_t determinize_work_limit_{0};
    bool do_determinization_{true};
};

} // namespace lucene
