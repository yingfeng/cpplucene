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

export module lucene_core:levenshtein_automata;

import :types;
import std;

import :automaton;
import :error;
import :parametric_description;

namespace lucene {

/// Constructs DFAs that match a word within some edit distance.
///
/// Implements the algorithm described in: Schulz and Mihov, "Fast String
/// Correction with Levenshtein Automata".
///
/// Port of `LevenshteinAutomata`
/// (`core/util/automation/levenshtein_automata.rs`).
export class LevenshteinAutomata {
public:
    /// Maximum edit distance this type can generate an automaton for.
    static constexpr int32_t MAXIMUM_SUPPORTED_DISTANCE = 2;

    /// A builder for `input`, optionally counting transpositions as a primitive
    /// edit.
    [[nodiscard]] static Result<LevenshteinAutomata> create(std::string_view input, bool with_transpositions);

    /// Expert variant of [`create`]: the word as codepoints, and the largest
    /// possible symbol of the alphabet.
    [[nodiscard]] static Result<LevenshteinAutomata> from_word(std::vector<int32_t> word, int32_t alpha_max,
                                                               bool with_transpositions);

    /// A DFA that accepts all strings within an edit distance of `n`, or
    /// `nullopt` when `n` is not supported.
    ///
    /// All such automata are deterministic, have no transitions to dead states
    /// and are not minimal.
    [[nodiscard]] Result<std::optional<Automaton>> to_automaton(size_t n) const;

    /// [`to_automaton`] with an exact prefix that every accepted string starts
    /// with.
    [[nodiscard]] Result<std::optional<Automaton>> to_automaton_with_prefix(size_t n, std::string_view prefix) const;

    /// The input word, as codepoints.
    [[nodiscard]] const std::vector<int32_t> &word() const noexcept { return word_; }
    /// The distinct symbols of the input word, sorted.
    [[nodiscard]] const std::vector<int32_t> &alphabet() const noexcept { return alphabet_; }
    /// The number of ranges outside of the alphabet.
    [[nodiscard]] size_t num_ranges() const noexcept { return num_ranges_; }
    /// The largest possible symbol of the alphabet.
    [[nodiscard]] int32_t alpha_max() const noexcept { return alpha_max_; }

private:
    LevenshteinAutomata() = default;

    /// The codepoints of `input`.
    [[nodiscard]] static Result<std::vector<int32_t>> code_points(std::string_view input);

    /// The characteristic vector `X(x, V)` where `V` is `word[pos, end)`.
    [[nodiscard]] int32_t get_vector(int32_t x, int32_t pos, int32_t end) const;

    /// Input word.
    std::vector<int32_t> word_{};
    /// The automata alphabet.
    std::vector<int32_t> alphabet_{};
    /// The maximum symbol in the alphabet.
    int32_t alpha_max_{0};
    /// Lower bounds for the ranges outside of the alphabet.
    std::vector<int32_t> range_lower_{};
    /// Upper bounds for the ranges outside of the alphabet.
    std::vector<int32_t> range_upper_{};
    /// The number of live entries of `range_lower_` and `range_upper_`.
    size_t num_ranges_{0};
    /// The descriptions of degrees 0 to 2; degree 0 has none.
    std::array<std::optional<ParametricDescription>, 3> descriptions_{};
};

} // namespace lucene
