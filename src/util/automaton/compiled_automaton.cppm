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

export module lucene_core:compiled_automaton;

import :types;
import std;

import :automaton;
import :byte_run_automaton;
import :byte_runnable;
import :bytes_ref;
import :bytes_ref_builder;
import :error;
import :nfa_run_automaton;
import :transition;

namespace lucene {

/// The internal form an [`Automaton`] was compiled into.
///
/// Port of `AutomatonType` (`core/util/automation/compiled_automaton.rs`).
export enum class AutomatonType : uint8_t {
    /// Automaton that accepts no strings.
    kNone = 0,
    /// Automaton that accepts all possible strings.
    kAll = 1,
    /// Automaton that accepts only a single fixed string.
    kSingle = 2,
    /// Catch-all for any other automaton.
    kNormal = 3,
};

/// Dispatches the runnable/transition operations to either a DFA
/// ([`ByteRunAutomaton`]) or an NFA ([`NFARunAutomaton`]).
///
/// Port of `AutomatonEnum` (`core/util/automation/compiled_automaton.rs`).
///
/// **Deviations:** rlucene boxes the NFA variant; the C++ port stores the
/// alternatives in two optionals, of which exactly one is engaged, because the
/// automaton is only an ephemeral accessor.
export class AutomatonEnum {
public:
    explicit AutomatonEnum(ByteRunAutomaton run) : run_(std::move(run)) {}
    explicit AutomatonEnum(NFARunAutomaton nfa) : nfa_(std::move(nfa)) {}

    /// `true` when this wraps a DFA.
    [[nodiscard]] bool is_byte() const noexcept { return run_.has_value(); }

    // ---- ByteRunnable ----

    [[nodiscard]] Result<int32_t> step(int32_t state, int32_t c);
    [[nodiscard]] Result<bool> is_accept(int32_t state) const;
    [[nodiscard]] size_t get_size() const;
    /// Whether `s[offset, offset + length)` is accepted.
    [[nodiscard]] Result<bool> run(std::span<const uint8_t> s, size_t offset, size_t length);

    // ---- TransitionAccessor ----

    [[nodiscard]] Result<int32_t> init_transition(int32_t state, Transition &t);
    [[nodiscard]] VoidResult get_next_transition(Transition &t);
    [[nodiscard]] Result<int32_t> get_num_transitions_with_state(int32_t state);
    [[nodiscard]] VoidResult get_transition(int32_t state, int32_t index, Transition &t);

private:
    std::optional<ByteRunAutomaton> run_{};
    std::optional<NFARunAutomaton> nfa_{};
};

/// An [`Automaton`] compiled into the most efficient internal form for the
/// language it accepts.
///
/// Port of `CompiledAutomaton` (`core/util/automation/compiled_automaton.rs`).
///
/// **Deviations:**
///   * `visit` is not ported yet: it needs `QueryVisitor` and `Query`, neither
///     of which exists in this port so far.
///   * `get_terms_enum` lives on `Terms` as `terms_enum_for`, because
///     `:compiled_automaton` importing `:terms` would be a module cycle.
///   * rlucene shares the common suffix through an `Arc`; the C++ port keeps an
///     owned copy.
///   * rlucene's `Accountable::ram_bytes_used` is not ported, as the C++ port
///     has no `Accountable` hierarchy.
export class CompiledAutomaton {
public:
    /// Creates a [`CompiledAutomaton`] with `finite = false` and
    /// `simplify = true`.
    [[nodiscard]] static Result<CompiledAutomaton> from_automaton(Automaton automaton);

    /// If `simplify` is `true`, possibly expensive operations are run to
    /// determine whether the automaton is one of the [`AutomatonType`] special
    /// cases.  Set `finite` to `true` when the automaton is finite, `false` when
    /// it is infinite or unknown.
    [[nodiscard]] static Result<CompiledAutomaton> create(Automaton automaton, bool finite, bool simplify);

    /// [`create`], for an automaton that already accepts bytes (`is_binary`) or
    /// for one whose labels are Unicode codepoints.
    [[nodiscard]] static Result<CompiledAutomaton> with_binary(Automaton automaton, bool finite, bool simplify,
                                                               bool is_binary);

    // ---- state ----

    /// The internal form; when `simplify` was requested this may be a special
    /// case, otherwise it is always [`AutomatonType::kNormal`].
    [[nodiscard]] AutomatonType type() const noexcept { return type_; }
    /// For [`AutomatonType::kSingle`], the singleton term.
    [[nodiscard]] const std::optional<BytesRef> &term() const noexcept { return term_; }
    /// Whether the automaton accepts a finite set of strings.
    [[nodiscard]] bool finite() const noexcept { return finite_; }
    /// Which state (of the compiled DFA), if any, accepts all suffixes; `-1`
    /// when there is none.
    [[nodiscard]] int32_t sink_state() const noexcept { return sink_state_; }
    /// The common suffix accepted by the automaton, for an infinite language
    /// with a non-empty common suffix; otherwise `nullopt`.
    [[nodiscard]] const std::optional<BytesRef> &common_suffix_ref() const noexcept { return common_suffix_; }
    /// The DFA, only valid for [`AutomatonType::kNormal`].
    [[nodiscard]] const std::optional<ByteRunAutomaton> &run_automaton() const noexcept { return run_automaton_; }
    /// The lazily determinizing NFA, only valid for [`AutomatonType::kNormal`]
    /// when the automaton was not deterministic.
    [[nodiscard]] const std::optional<NFARunAutomaton> &nfa_run_automaton() const noexcept {
        return nfa_run_automaton_;
    }

    /// The compiled automaton as a [`ByteRunnable`]/[`AutomatonEnum`].
    [[nodiscard]] Result<AutomatonEnum> get_automaton() const;

    /// Finds the largest term accepted by this automaton that is `<=` `input`.
    ///
    /// The result is placed in `output`; `output` may alias `input`.  Returns
    /// `nullopt` when there is no floor term, i.e. `input` sorts before the
    /// first accepted term.
    [[nodiscard]] Result<std::optional<BytesRef>> floor(const BytesRef &input, BytesRefBuilder &output);

    /// Structural equality, mirroring rlucene's `PartialEq`: the special cases
    /// compare by type only, [`AutomatonType::kSingle`] additionally by term and
    /// [`AutomatonType::kNormal`] additionally by DFA (or NFA) content.
    [[nodiscard]] bool operator==(const CompiledAutomaton &other) const;
    [[nodiscard]] bool operator!=(const CompiledAutomaton &other) const { return !(*this == other); }

private:
    CompiledAutomaton() = default;

    /// The state that accepts every suffix, or `-1`.
    [[nodiscard]] static Result<int32_t> find_sink_state(const Automaton &automaton);

    /// Fills `term` with the smallest accepted string strictly below
    /// `lead_label`, then with the largest label of each following state.
    [[nodiscard]] Result<BytesRef> add_tail(int32_t state, BytesRefBuilder &term, size_t idx, int32_t lead_label);

    AutomatonType type_{AutomatonType::kNormal};
    std::optional<BytesRef> term_{};
    std::optional<ByteRunAutomaton> run_automaton_{};
    std::optional<NFARunAutomaton> nfa_run_automaton_{};
    std::optional<BytesRef> common_suffix_{};
    bool finite_{false};
    int32_t sink_state_{-1};
    /// Scratch transition reused by [`floor`] and [`add_tail`].
    Transition transition_{};
};

} // namespace lucene
