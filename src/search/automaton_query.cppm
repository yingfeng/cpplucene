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

export module lucene_core:automaton_query;

import :types;
import std;

import :automaton;
import :compiled_automaton;
import :error;
import :multi_term_query;
import :term;
import :terms;
import :terms_enum;

namespace lucene {

/// A [`MultiTermQuery`] that matches the terms accepted by a finite-state
/// machine.
///
/// Port of `AutomatonQuery` (`core/search/automaton_query.rs`).
///
/// The term's text is only used for `to_string` and `equals`; the terms that
/// match are the ones the compiled automaton accepts.
///
/// **Deviations** (C++ mechanics and scope):
///  * construction is fallible (compiling the automaton can fail), so the
///    reference implementation's `new` becomes the [`create`] factory returning
///    `Result`;
///  * the reference implementation's `Accountable::ram_bytes_used` is not ported,
///    because this port has no `Accountable` hierarchy.
export class AutomatonQuery : public MultiTermQuery {
public:
    /// [`create`] with the reference implementation's default rewrite method and
    /// `is_binary = false`.
    [[nodiscard]] static Result<AutomatonQuery> from_automaton(Term term, Automaton automaton);

    /// [`create`] with `is_binary` given; `is_binary` means the automaton already
    /// accepts bytes and must not go through the UTF-32 to UTF-8 conversion.
    [[nodiscard]] static Result<AutomatonQuery> from_automaton_with_binary(Term term, Automaton automaton,
                                                                          bool is_binary);

    /// A query for the terms of `term`'s field that `automaton` accepts.
    [[nodiscard]] static Result<AutomatonQuery> create(Term term, Automaton automaton, bool is_binary,
                                                      MultiTermRewriteMethod rewrite_method);

    /// The term whose text names the pattern of this query.
    [[nodiscard]] const Term &term() const noexcept { return term_; }

    /// The automaton compiled for this query.
    [[nodiscard]] const CompiledAutomaton &compiled() const noexcept { return compiled_; }

    // ---- Query ----

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

protected:
    AutomatonQuery(Term term, CompiledAutomaton compiled, MultiTermRewriteMethod rewrite_method);

    /// Compiles `automaton` into the form the term dictionary can intersect
    /// against, converting a code point automaton to bytes unless `is_binary`.
    [[nodiscard]] static Result<CompiledAutomaton> compile(const Automaton &automaton, bool is_binary);

    // ---- MultiTermQuery ----

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> terms_enum(Terms &terms) const override;

    [[nodiscard]] std::string_view query_name() const override { return "AutomatonQuery"; }

private:
    Term term_;
    CompiledAutomaton compiled_;
};

} // namespace lucene
