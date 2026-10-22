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

export module lucene_core:automata;

import :types;
import std;

import :automaton;
import :bytes_ref;
import :bytes_ref_iterator;
import :error;
import :strings_to_automaton;

namespace lucene {

/// Construction of basic automata.
///
/// Port of `Automata` (`core/util/automation/automata.rs`).
///
/// **Deviation:** rlucene spells the optional interval bounds
/// `Option<&BytesRef>`; here a null pointer means "no bound" and the
/// corresponding inclusive flag must then be `true`, exactly like the Rust
/// version.
export class Automata {
public:
    /// [`make_string_union`] limits terms to this maximum length to ensure the
    /// stack does not overflow while building, since the algorithm currently
    /// relies on recursion.
    static constexpr int32_t MAX_STRING_UNION_TERM_LENGTH = StringsToAutomaton::MAX_STRING_UNION_TERM_LENGTH;

    /// A new (deterministic) automaton with the empty language.
    [[nodiscard]] static Result<Automaton> make_empty();
    /// A new (deterministic) automaton that accepts only the empty string.
    [[nodiscard]] static Result<Automaton> make_empty_string();
    /// A new (deterministic) automaton that accepts all Unicode strings.
    [[nodiscard]] static Result<Automaton> make_any_string();
    /// A new (deterministic) automaton that accepts all binary terms
    /// (`0..=255`).
    [[nodiscard]] static Result<Automaton> make_any_binary();
    /// A new (deterministic) automaton that accepts all binary terms except the
    /// empty string.
    [[nodiscard]] static Result<Automaton> make_non_empty_binary();
    /// A new (deterministic) automaton that accepts any single Unicode
    /// codepoint.
    [[nodiscard]] static Result<Automaton> make_any_char();
    /// Appends a transition accepting any single Unicode codepoint from `state`
    /// and returns the new state.
    [[nodiscard]] static Result<int32_t> append_any_char(Automaton &a, int32_t state);
    /// A new (deterministic) automaton that accepts a single codepoint with the
    /// given value.
    [[nodiscard]] static Result<Automaton> make_char(int32_t c);
    /// Appends a transition accepting `c` from `state` and returns the new
    /// state.
    [[nodiscard]] static Result<int32_t> append_char(Automaton &a, int32_t state, int32_t c);
    /// A new (deterministic) automaton that accepts a single codepoint within
    /// `[min, max]`.
    [[nodiscard]] static Result<Automaton> make_char_range(int32_t min, int32_t max);

    /// A new deterministic, minimal automaton accepting all binary terms in the
    /// given interval.
    ///
    /// Unlike [`make_decimal_interval`] the returned automaton is infinite,
    /// because terms behave like floating point numbers leading with a decimal
    /// point.  When `min == max` and both are inclusive, the automaton is
    /// finite and accepts exactly that one term.
    [[nodiscard]] static Result<Automaton> make_binary_interval(const BytesRef *min, bool min_inclusive,
                                                               const BytesRef *max, bool max_inclusive);
    /// A new automaton that accepts strings representing decimal (base 10)
    /// non-negative integers in `[min, max]`.
    ///
    /// When `digits` is greater than `0` a fixed number of digits is used
    /// (strings are prefixed by `0`s to obtain the right length); otherwise any
    /// number of leading `0`s is accepted.
    [[nodiscard]] static Result<Automaton> make_decimal_interval(int32_t min, int32_t max, int32_t digits);

    /// A new (deterministic) automaton that accepts exactly `s`.
    [[nodiscard]] static Result<Automaton> make_string(std::string_view s);
    /// A new (deterministic) automaton that accepts exactly `term`.
    [[nodiscard]] static Result<Automaton> make_binary(const BytesRef &term);
    /// A new (deterministic) automaton that accepts exactly `word[offset,
    /// offset + length)` given as Unicode codepoints.
    [[nodiscard]] static Result<Automaton> make_string_from_codepoints(std::span<const int32_t> word, size_t offset,
                                                                      size_t length);

    /// A new (deterministic and minimal) automaton accepting the union of the
    /// given UTF-8 encoded strings, which must be in sorted order.  The result
    /// is codepoint based.
    [[nodiscard]] static Result<Automaton> make_string_union(std::span<const BytesRef> utf8_strings);
    /// [`make_string_union`] with binary (byte) transition labels.
    [[nodiscard]] static Result<Automaton> make_binary_string_union(std::span<const BytesRef> utf8_strings);
    /// [`make_string_union`] over an iterator of sorted terms.
    [[nodiscard]] static Result<Automaton> make_string_union_from_iter(BytesRefIterator &utf8_strings);
    /// [`make_binary_string_union`] over an iterator of sorted terms.
    [[nodiscard]] static Result<Automaton> make_binary_string_union_from_iter(BytesRefIterator &utf8_strings);

    /// Sub-automaton accepting every decimal number of length
    /// `x.substr(n).length()` (`between` needs it).
    [[nodiscard]] static Result<int32_t> any_of_right_length(AutomatonBuilder &builder, std::string_view x, size_t n);
    /// Sub-automaton accepting the decimal numbers of value at least `x[n..]`
    /// and of length `x[n..].length()`.
    [[nodiscard]] static Result<int32_t> at_least(AutomatonBuilder &builder, std::string_view x, size_t n,
                                                  std::vector<int32_t> &initials, bool zeros);
    /// Sub-automaton accepting the decimal numbers of value at most `x[n..]`.
    [[nodiscard]] static Result<int32_t> at_most(AutomatonBuilder &builder, std::string_view x, size_t n);
    /// Sub-automaton accepting the decimal numbers between `x[n..]` and
    /// `y[n..]`, whose lengths must match.
    [[nodiscard]] static Result<int32_t> between(AutomatonBuilder &builder, std::string_view x, std::string_view y,
                                                 size_t n, std::vector<int32_t> &initials, bool zeros);

    /// `true` when `from` onward of `br` is all zeros.
    [[nodiscard]] static bool suffix_is_zeros(const BytesRef &br, size_t from);
};

} // namespace lucene
