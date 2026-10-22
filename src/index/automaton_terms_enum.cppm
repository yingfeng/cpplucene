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

export module lucene_core:automaton_terms_enum;

import :types;
import std;

import :bytes_ref;
import :bytes_ref_builder;
import :compiled_automaton;
import :error;
import :filtered_terms_enum;
import :ints_ref;
import :terms_enum;
import :transition;

namespace lucene {

/// A [`FilteredTermsEnum`] hook that enumerates the terms accepted by a DFA.
///
/// The algorithm:
///  1. as long as matches are successful, keep reading sequentially;
///  2. when a match fails, skip to the next string in lexicographic order that
///     does not enter a reject state.
///
/// **Note:** the algorithm does not attempt to skip directly to the next
/// completely accepted string, which is not possible when the language accepted
/// by the DFA is infinite (for example because of the `*` operator).
///
/// Port of `AutomatonTermsEnum` (`core/index/automaton_terms_enum.rs`).
export class AutomatonTermsEnum final : public FilteredTermsEnumBase {
public:
    /// Wraps `tenum` so that only terms accepted by `compiled` are returned.
    ///
    /// `compiled` must be of [`AutomatonType::kNormal`]; use
    /// [`Terms::terms_enum_for`] for the specialized forms.  `start_term` is the
    /// term to start enumerating from, when non-null.
    [[nodiscard]] static Result<std::unique_ptr<FilteredTermsEnum>>
    create(std::unique_ptr<TermsEnum> tenum, const CompiledAutomaton &compiled, const BytesRef *start_term);

    // ---- FilteredTermsEnumBase ----

    [[nodiscard]] Result<AcceptStatus> accept(const BytesRef &term, int64_t ord) override;
    [[nodiscard]] Result<std::optional<BytesRef>> next_seek_term(const BytesRef *term) override;

    /// Increments `seek_bytes_ref` to the next string in binary order after the
    /// current one that will not put the machine into a reject state; `false`
    /// when no such string exists.
    ///
    /// Correctness depends on the automaton being deterministic and having no
    /// transitions to dead states.
    [[nodiscard]] Result<bool> next_string();

private:
    AutomatonTermsEnum(AutomatonEnum automaton, std::optional<BytesRef> common_suffix, bool finite,
                       std::optional<BytesRef> start_term);

    /// Records that `state` has been visited in the current generation.
    [[nodiscard]] VoidResult set_visited(int32_t state);
    /// Whether `state` has already been visited in the current generation.
    [[nodiscard]] bool is_visited(int32_t state) const;

    /// Switches to linear enumeration after detecting a looping transition at
    /// `position`, bounding the range of terms read sequentially.
    [[nodiscard]] VoidResult set_linear(size_t position);

    /// [`next_string`], continuing from `state` at `position`.
    [[nodiscard]] Result<bool> next_string_with_position(int32_t state, size_t position);

    /// Backtracks from `position`; `nullopt` when all solutions are exhausted.
    [[nodiscard]] std::optional<size_t> backtrack(size_t position);

    /// A tabled, byte oriented form of the DFA.
    AutomatonEnum automaton_;
    /// Common suffix of the automaton, when it has one.
    std::optional<BytesRef> common_suffix_;
    /// Whether the automaton accepts a finite language.
    bool finite_{false};
    /// Generation counter per state, so that the table does not have to be
    /// cleared: `visited_[state] == cur_gen_` means "visited in this pass".
    std::vector<uint16_t> visited_{};
    uint16_t cur_gen_{0};
    /// The reference used for seeking forward through the term dictionary.
    BytesRefBuilder seek_bytes_ref_{};
    /// Whether an infinite portion of the DFA is being enumerated; in that case
    /// it is faster to drive the query from the terms dictionary, and
    /// `linear_upper_bound_` is the end of the range to read sequentially.
    bool linear_{false};
    BytesRef linear_upper_bound_{};
    Transition transition_{};
    IntsRefBuilder saved_states_{};
    /// The term to start enumerating from.
    std::optional<BytesRef> start_term_{};
};

} // namespace lucene
