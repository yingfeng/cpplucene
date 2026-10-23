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

export module lucene_core:fuzzy_query;

import :types;
import std;

import :compiled_automaton;
import :error;
import :levenshtein_automata;
import :multi_term_query;
import :term;
import :terms;
import :terms_enum;

namespace lucene {

/// A fuzzy search: terms within a Damerau-Levenshtein (optimal string
/// alignment) edit distance of the given term, or within the classic
/// Levenshtein distance when transpositions do not count as an edit.
///
/// Port of `FuzzyQuery` (`core/search/fuzzy_query.rs`).
///
/// At most [`LevenshteinAutomata::MAXIMUM_SUPPORTED_DISTANCE`] edits are
/// supported; larger distances match a large part of the term dictionary and are
/// better served by an n-gram index.  Note that one or two character terms
/// sometimes do not match, because the edit distance has to be smaller than the
/// shorter of the two terms: `"abcd"` with `max_edits = 2` does not match `"ab"`,
/// and `"a"` with `max_edits = 2` does not match `"abc"`.
///
/// **Deviations** (C++ scope):
///  * the reference implementation's constructors become static factories
///    returning `Result`, because building the automaton can fail;
///  * the reference implementation's default top-terms blended frequency rewrite is not
///    ported, so [`max_expansions`] cannot cap the number of matching terms and
///    the rewrite method is one of the [`MultiTermRewriteMethod`] values;
///  * the terms are enumerated with the automaton of the full edit distance
///    instead of the reference implementation's incremental `FuzzyTermsEnum`; the
///    accepted terms are the same, but no per-term boosted score is attached.
export class FuzzyQuery final : public MultiTermQuery {
public:
    static constexpr int32_t DEFAULT_MAX_EDITS = LevenshteinAutomata::MAXIMUM_SUPPORTED_DISTANCE;
    static constexpr size_t DEFAULT_PREFIX_LENGTH = 0;
    static constexpr size_t DEFAULT_MAX_EXPANSIONS = 50;
    static constexpr bool DEFAULT_TRANSPOSITIONS = true;

    /// A query for the terms within [`DEFAULT_MAX_EDITS`] edits of `term`.
    [[nodiscard]] static Result<FuzzyQuery> create(Term term);

    /// [`create`], with the maximum edit distance given.
    [[nodiscard]] static Result<FuzzyQuery> with_max_edits(Term term, int32_t max_edits);

    /// [`with_max_edits`], requiring a common prefix of `prefix_length`
    /// characters.
    [[nodiscard]] static Result<FuzzyQuery> with_max_edits_and_prefix(Term term, int32_t max_edits,
                                                                     size_t prefix_length);

    /// [`with_max_edits_and_prefix`], with the expansion cap and whether
    /// transpositions count as one edit.
    [[nodiscard]] static Result<FuzzyQuery> with_options(Term term, int32_t max_edits, size_t prefix_length,
                                                       size_t max_expansions, bool transpositions);

    /// [`with_options`], with the rewrite method to expand the matching terms
    /// with.
    [[nodiscard]] static Result<FuzzyQuery> with_rewrite(Term term, int32_t max_edits, size_t prefix_length,
                                                       size_t max_expansions, bool transpositions,
                                                       MultiTermRewriteMethod rewrite_method);

    /// The automaton that accepts the terms within `max_edits` edits of `term`.
    ///
    /// This is a very low level entry point; it may disappear when the
    /// implementation of fuzzy matching changes.
    [[nodiscard]] static Result<CompiledAutomaton> get_fuzzy_automaton(std::string term, int32_t max_edits,
                                                                      size_t prefix_length, bool transpositions);

    /// The largest edit distance whose similarity is at least
    /// `minimum_similarity` for a term of `term_len` characters.
    [[nodiscard]] static int32_t float_to_edits(float minimum_similarity, size_t term_len);

    /// The term this query matches fuzzily.
    [[nodiscard]] const Term &term() const noexcept { return term_; }
    /// The maximum number of edits a matching term may differ by.
    [[nodiscard]] int32_t max_edits() const noexcept { return max_edits_; }
    /// The number of leading characters that must match exactly.
    [[nodiscard]] size_t prefix_length() const noexcept { return prefix_length_; }
    /// The maximum number of terms to expand to; see the class note.
    [[nodiscard]] size_t max_expansions() const noexcept { return max_expansions_; }
    /// Whether transpositions count as a primitive edit operation.
    [[nodiscard]] bool transpositions() const noexcept { return transpositions_; }

    /// The automaton this query matches terms with.
    [[nodiscard]] Result<CompiledAutomaton> automaton() const;

    // ---- Query ----

    /// `field:term~<max_edits>`, the form the reference implementation prints.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

protected:
    [[nodiscard]] std::string_view query_name() const override { return "FuzzyQuery"; }

    [[nodiscard]] bool peers(const MultiTermQuery &other) const override;

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> terms_enum(Terms &terms) const override;

private:
    FuzzyQuery(Term term, int32_t max_edits, size_t prefix_length, size_t max_expansions, bool transpositions,
               MultiTermRewriteMethod rewrite_method);

    Term term_;
    int32_t max_edits_{0};
    size_t prefix_length_{0};
    size_t max_expansions_{0};
    bool transpositions_{true};
};

} // namespace lucene
