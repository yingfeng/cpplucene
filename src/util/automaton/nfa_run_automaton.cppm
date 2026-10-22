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

export module lucene_core:nfa_run_automaton;

import :types;
import std;

import :automaton;
import :byte_runnable;
import :error;
import :point_transition_set;
import :state_set;
import :transition;

namespace lucene {

/// A runnable automaton that does not require a precomputed DFA: it
/// determinizes lazily, on demand, and memorizes the DFA states it has explored.
///
/// **Note:** the current implementation is **not thread safe**.
///
/// Port of `NFARunAutomaton` (`core/util/automation/nfa_run_automaton.rs`),
/// implemented after <https://swtch.com/~rsc/regexp/regexp1.html>.
///
/// **Deviations:** rlucene shares the automaton and the class tables through
/// `Arc` so that the type is cheap to clone; the C++ port owns them, and the
/// DFA state map keeps a copy of the NFA state set instead of an `Arc<Vec>`.
export class NFARunAutomaton final : public ByteRunnable {
public:
    /// The result of a transition that does not exist.
    static constexpr int32_t MISSING = -1;
    /// The result of a transition that has not been computed yet.
    static constexpr int32_t NOT_COMPUTED = -2;

    /// Builds the automaton over the full Unicode codepoint alphabet.
    [[nodiscard]] static Result<NFARunAutomaton> create(Automaton automaton);
    /// Builds the automaton over an alphabet of `alphabet_size` codepoints.
    [[nodiscard]] static Result<NFARunAutomaton> create_with_alphabet_size(Automaton automaton, int32_t alphabet_size);

    /// Whether the given codepoints are accepted; mainly a test helper.
    [[nodiscard]] Result<bool> run_codepoints(std::span<const int32_t> input);

    // ---- ByteRunnable ----

    [[nodiscard]] Result<int32_t> step(int32_t state, int32_t c) override;
    [[nodiscard]] Result<bool> is_accept(int32_t state) const override;
    [[nodiscard]] size_t get_size() const override { return dstate_slots_; }

    // ---- TransitionAccessor over the lazily computed DFA ----

    /// Starts iterating the transitions of `state`; the number of transitions
    /// is returned.
    [[nodiscard]] Result<int32_t> init_transition(int32_t state, Transition &t);
    /// Reads the next transition of the current iteration.
    [[nodiscard]] VoidResult get_next_transition(Transition &t);
    /// Number of outgoing transitions of `state`.
    [[nodiscard]] Result<int32_t> get_num_transitions_with_state(int32_t state);
    /// Reads transition `index` of `state`.
    [[nodiscard]] VoidResult get_transition(int32_t state, int32_t index, Transition &t);

    /// The codepoint class of `c`.
    [[nodiscard]] Result<size_t> get_char_class(int32_t c) const;

    /// The automaton this was built from.
    [[nodiscard]] const Automaton &automaton() const noexcept { return automaton_; }

private:
    /// The identity of a lazily built DFA state: its NFA state set, plus the
    /// hash of that set to keep equality checks cheap.
    struct DStateKey {
        std::vector<int32_t> nfa_states{};
        int32_t hash_code{0};

        [[nodiscard]] bool operator==(const DStateKey &other) const noexcept {
            return hash_code == other.hash_code && nfa_states == other.nfa_states;
        }
    };

    struct DStateKeyHash {
        [[nodiscard]] size_t operator()(const DStateKey &key) const noexcept {
            return std::hash<int32_t>{}(key.hash_code);
        }
    };

    /// One lazily built DFA state.
    struct DState {
        /// The NFA states this DFA state stands for.
        std::vector<int32_t> nfa_states{};
        /// The DFA transition table, `points_.size()` entries, `NOT_COMPUTED`
        /// until known.
        std::vector<int32_t> transitions{};
        /// Ring hash of `nfa_states`.
        uint32_t hash_code{0};
        /// Whether any of the NFA states accepts.
        bool is_accept{false};
        /// Reusable transition, only used while stepping.
        Transition step_transition{};
        /// The label interval every char class in the last step shares, when it
        /// is wider than a single class.
        std::optional<Transition> minimal_transition{};
        /// How many entries of `transitions` have been computed.
        size_t computed_transitions{0};
        /// How many entries of `transitions` are not `MISSING`.
        int32_t outgoing_transitions{0};

        /// Records `dest` for `char_class`, unless it is already known.
        void assign_transition(size_t char_class, int32_t dest) {
            if (transitions[char_class] == NOT_COMPUTED) {
                computed_transitions += 1;
                transitions[char_class] = dest;
                if (dest != MISSING) {
                    outgoing_transitions += 1;
                }
            }
        }

        /// Grows the transition table to `points_len` entries when needed.
        void init_transitions(size_t points_len) {
            if (transitions.size() < points_len) {
                transitions.resize(points_len, NOT_COMPUTED);
            }
        }
    };

    /// The reusable state of the determinization algorithm.
    struct State {
        std::unordered_map<DStateKey, int32_t, DStateKeyHash> dstate_to_ord{};
        PointTransitionSet transition_set{};
    };

    NFARunAutomaton() = default;

    [[nodiscard]] VoidResult init(Automaton automaton, int32_t alphabet_size);

    /// Builds the DFA state standing for `nfa_states`.
    [[nodiscard]] DState make_dstate(std::vector<int32_t> nfa_states) const;

    [[nodiscard]] Result<int32_t> step_with_dstate_index(size_t dstate_index, int32_t c);
    /// The ordinal of `dstate`, adding it when it is new.
    [[nodiscard]] Result<int32_t> find_dstate(std::optional<DState> dstate);
    [[nodiscard]] VoidResult set_transition_accordingly(Transition &t) const;
    [[nodiscard]] Result<int32_t> next_state(size_t char_class, size_t index);
    /// Given the NFA states of DFA state `index` and a codepoint, computes the
    /// NFA states of the next DFA state.
    [[nodiscard]] Result<std::optional<DState>> step_with_index(int32_t c, size_t index);
    /// Fills in the transition table of DFA state `index` for the whole alphabet.
    [[nodiscard]] VoidResult determinize(size_t index);

    Automaton automaton_{};
    /// The codepoint class interval start points.
    std::vector<int32_t> points_{};
    /// Map from a codepoint below 256 to its class.
    std::vector<size_t> classmap_{};
    int32_t alphabet_size_{0};
    std::vector<DState> dstates_{};
    /// The historical slot count, as reported by [`get_size`].
    size_t dstate_slots_{0};
    State state_{};
    StateSet states_set_{5};
};

} // namespace lucene
