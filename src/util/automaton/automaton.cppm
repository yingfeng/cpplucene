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

export module lucene_core:automaton;

import :types;
import std;

import :error;
import :transition;
import :transition_accessor;

namespace lucene {

/// The largest Unicode code point, `Character::MAX_CODE_POINT`.
inline constexpr int32_t MAX_CODE_POINT = 0x10FFFF;

/// Finite-state automaton and all its states and transitions.
///
/// Port of `Automaton` (`core/util/automation/automaton.rs`).
///
/// States are integers created with [`create_state`]; state `0` is the initial
/// state.  Mark a state as accepting with [`set_accept`], add transitions with
/// [`add_transition`].
///
/// Each state must have all of its transitions added at once; when that is too
/// restrictive use [`AutomatonBuilder`].  Once a state is finished (either
/// because transitions are being added to another state, or because
/// [`finish_state`] was called) its transitions are sorted by min, max, dest and
/// adjacent ranges going to the same destination are merged.
///
/// **Deviation:** the reference implementation's `ram_bytes_used` (`Accountable`) is not ported, as
/// the C++ port has no `Accountable` hierarchy.
export class Automaton final : public TransitionAccessor {
public:
    Automaton();
    /// An automaton with room for `num_states` states and `num_transitions`
    /// transitions.
    [[nodiscard]] static Automaton with_capacity(size_t num_states, size_t num_transitions);

    /// Creates a new state and returns it; its transitions must be added
    /// before any other state's.
    [[nodiscard]] int32_t create_state();

    /// Sets or clears `state` as an accepting state.
    void set_accept(int32_t state, bool accept);
    /// Whether `state` is an accepting state.
    [[nodiscard]] bool is_accept(int32_t state) const;

    /// Every transition of every state; object heavy, iterating state by state
    /// is cheaper.
    [[nodiscard]] std::vector<std::vector<Transition>> get_sorted_transitions() const;

    /// Adds a transition accepting only `label`.
    [[nodiscard]] VoidResult add_transition_label(int32_t source, int32_t dest, int32_t label);
    /// Adds a transition accepting `[min, max]`.
    [[nodiscard]] VoidResult add_transition(int32_t source, int32_t dest, int32_t min, int32_t max);
    /// Adds a virtual epsilon transition: the transitions of `dest` are copied
    /// onto `source`, so `dest` must already be complete.
    [[nodiscard]] VoidResult add_epsilon(int32_t source, int32_t dest);
    /// Copies all states and transitions of `other`, renumbering them.
    [[nodiscard]] VoidResult copy(const Automaton &other);

    /// `true` when no state has two transitions leaving with the same label.
    [[nodiscard]] bool is_deterministic() const noexcept { return deterministic_; }

    /// Freezes the current state; needed once for the last state whose
    /// transitions were added.
    [[nodiscard]] VoidResult finish_state();

    [[nodiscard]] int32_t get_num_states() const noexcept { return static_cast<int32_t>(next_state_ / 2); }
    [[nodiscard]] int32_t get_num_transitions() const noexcept { return static_cast<int32_t>(next_transition_ / 3); }

    /// Sorted array of all interval start points of the accepted labels.
    [[nodiscard]] std::vector<int32_t> get_start_points() const;

    /// Lookup in the transitions, assuming determinism: the destination state,
    /// or `-1` when no transition matches `label`.
    [[nodiscard]] int32_t step(int32_t state, int32_t label) const;
    /// Like [`step`], but continues from `transition.transition_upto` so that
    /// iterating the labels of one state stays efficient.  `transition.dest` is
    /// set to the destination, or to `-1` when nothing matches.
    [[nodiscard]] int32_t next(Transition &transition, int32_t label) const;

    /// Structural equality: same states, same accepting states, same
    /// transitions.
    [[nodiscard]] bool operator==(const Automaton &other) const;
    [[nodiscard]] bool operator!=(const Automaton &other) const { return !(*this == other); }

    /// Compact string form of the transitions, for debugging and tests.
    [[nodiscard]] std::string to_dot() const;

    // TransitionAccessor
    [[nodiscard]] int32_t init_transition(int32_t state, Transition &t) const override;
    [[nodiscard]] VoidResult get_next_transition(Transition &t) const override;
    [[nodiscard]] int32_t get_num_transitions_with_state(int32_t state) const override;
    void get_transition(int32_t state, int32_t index, Transition &t) const override;

private:
    void grow_states();
    void grow_transitions();
    [[nodiscard]] VoidResult finish_current_state(size_t state);
    [[nodiscard]] bool transition_sorted(const Transition &t) const;
    [[nodiscard]] int32_t next_impl(int32_t state, int32_t from_transition_index, int32_t label,
                                    Transition *transition) const;

    /// Index into `states_` where the next state is written; it advances by two
    /// per state because each state packs a pointer into `transitions_` and a
    /// count of its outgoing transitions.
    size_t next_state_{0};
    /// Index into `transitions_` where the next transition is written; it
    /// advances by three per transition (`dest`, `min`, `max`).
    size_t next_transition_{0};
    /// The state whose transitions are being added; the caller must add all of
    /// them before moving on.
    std::optional<size_t> cur_state_{};
    /// Two slots per state: the index of its first transition (`-1` while none
    /// were added) and the number of its transitions.
    std::vector<int32_t> states_{};
    std::vector<bool> is_accept_{};
    std::vector<int32_t> transitions_{};
    bool deterministic_{true};
};

/// Records new states and transitions and then builds the [`Automaton`].
///
/// Port of `Builder` (`core/util/automation/automaton.rs`).
///
/// Use it when an automaton cannot be built directly because adding all
/// transitions of a state at once is too restrictive.
export class AutomatonBuilder {
public:
    AutomatonBuilder();
    /// A builder with room for `num_states` states and `num_transitions`
    /// transitions.
    [[nodiscard]] static AutomatonBuilder with_capacity(size_t num_states, size_t num_transitions);

    /// Adds a transition accepting only `label`.
    [[nodiscard]] VoidResult add_transition_label(int32_t source, int32_t dest, int32_t label);
    /// Adds a transition accepting `[min, max]`.
    [[nodiscard]] VoidResult add_transition(int32_t source, int32_t dest, int32_t min, int32_t max);
    /// Adds a virtual epsilon transition, copying the transitions of `dest`.
    [[nodiscard]] VoidResult add_epsilon(int32_t source, int32_t dest);

    /// Compiles the added states and transitions into an [`Automaton`].
    [[nodiscard]] Result<Automaton> finish();

    [[nodiscard]] int32_t create_state();
    void set_accept(int32_t state, bool accept);
    [[nodiscard]] bool is_accept(int32_t state) const;
    [[nodiscard]] int32_t get_num_states() const noexcept { return next_state_; }

    /// Copies all states and transitions of `other`, renumbering them.
    [[nodiscard]] VoidResult copy(const Automaton &other);
    /// Copies only the states (and their accept flags) of `other`.
    void copy_states(const Automaton &other);

private:
    int32_t next_state_{0};
    std::vector<bool> is_accept_{};
    /// Four slots per transition: source, dest, min, max.
    std::vector<int32_t> transitions_{};
    size_t next_transition_{0};
};

} // namespace lucene
