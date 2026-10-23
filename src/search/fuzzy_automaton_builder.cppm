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

export module lucene_core:fuzzy_automaton_builder;

import :types;
import std;

import :automaton;
import :compiled_automaton;
import :error;
import :levenshtein_automata;

namespace lucene {

/// Builds the compiled automata a fuzzy match on one term needs, for a given
/// maximum edit distance, a fixed prefix and whether transpositions count as one
/// edit.
///
/// Port of `FuzzyAutomatonBuilder` (`core/search/fuzzy_automaton_builder.rs`).
export class FuzzyAutomatonBuilder {
public:
    /// A builder for `term`; `prefix_length` is clipped to the length of the
    /// term, so a prefix longer than the term demands the whole term.
    [[nodiscard]] static Result<FuzzyAutomatonBuilder> create(std::string term, int32_t max_edits,
                                                             size_t prefix_length, bool transpositions);

    /// One compiled automaton per edit distance, from `0` up to [`max_edits`].
    [[nodiscard]] Result<std::vector<CompiledAutomaton>> build_automaton_set() const;

    /// The compiled automaton for the full edit distance.
    [[nodiscard]] Result<CompiledAutomaton> build_max_edit_automaton() const;

    /// The term this builder was created for.
    [[nodiscard]] const std::string &term() const noexcept { return term_; }
    /// The number of code points of the term.
    [[nodiscard]] size_t term_length() const noexcept { return term_length_; }
    /// The maximum edit distance.
    [[nodiscard]] int32_t max_edits() const noexcept { return max_edits_; }

private:
    FuzzyAutomatonBuilder(std::string term, int32_t max_edits, LevenshteinAutomata lev_builder, std::string prefix,
                          size_t term_length);

    /// The automaton for `edits`, with the fixed prefix.
    [[nodiscard]] Result<CompiledAutomaton> compile(size_t edits) const;

    std::string term_;
    int32_t max_edits_{0};
    LevenshteinAutomata lev_builder_;
    std::string prefix_;
    size_t term_length_{0};
};

} // namespace lucene
