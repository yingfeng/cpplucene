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

export module lucene_core:strings_to_automaton;

import :types;
import std;

import :automaton;
import :bytes_ref;
import :bytes_ref_builder;
import :bytes_ref_iterator;
import :error;

namespace lucene {

/// Builds a minimal, deterministic [`Automaton`] that accepts a set of strings
/// using the algorithm described in "Incremental Construction of Minimal Acyclic
/// Finite-State Automata" by Daciuk, Mihov, Watson and Watson.
///
/// This requires sorted input data, but is very fast (nearly linear with the
/// input size).  It also offers the ability to directly build a binary
/// [`Automaton`] representation.  Users should access this functionality
/// through [`Automata`](lucene_core:automata).
///
/// Port of `StringsToAutomaton` (`core/util/automation/strings_to_automaton.rs`).
///
/// **Deviation:** the state registry is an ordered map instead of a hash map.
/// The algorithm only asks it for exact matches, so the result is identical,
/// and the ordered map saves writing a hash function over the state.
export class StringsToAutomaton {
public:
    /// [`Automata::make_string_union`] limits terms to this maximum length to
    /// ensure the stack does not overflow while building, since the algorithm
    /// currently relies on recursion.
    static constexpr int32_t MAX_STRING_UNION_TERM_LENGTH = 1000;

    StringsToAutomaton();

    /// Builds a minimal, deterministic automaton from a sorted list of
    /// [`BytesRef`] representing UTF-8 strings.  Creates an [`Automaton`] with
    /// UTF-8 codepoint transition labels, or binary labels when `as_binary`.
    [[nodiscard]] static Result<Automaton> build(std::span<const BytesRef> input, bool as_binary);
    /// [`build`] over an iterator of sorted terms.
    [[nodiscard]] static Result<Automaton> build_from_iterator(BytesRefIterator &input, bool as_binary);

private:
    /// One state of the automaton under construction.
    struct State {
        /// Labels of the outgoing transitions, sorted.
        std::vector<int32_t> labels;
        /// Targets of the outgoing transitions, indexed like `labels`.
        std::vector<size_t> states;
        /// `true` when this state is the end of at least one input sequence.
        bool is_final{false};

        /// The target of the transition labelled `label`, when there is one.
        [[nodiscard]] std::optional<size_t> get_state(int32_t label) const;
        /// `true` when this state has any outgoing transition.
        [[nodiscard]] bool has_children() const noexcept { return !labels.empty(); }
        /// Adds the transition `label -> new_state`.
        size_t new_state(int32_t label, size_t new_state_index);
        /// The target of the most recently added transition.
        [[nodiscard]] Result<size_t> last_child() const;
        /// The target of the most recently added transition, when it is labelled
        /// `label`.
        [[nodiscard]] std::optional<size_t> last_child_with_label(int32_t label) const;
        /// Retargets the most recently added transition.
        void replace_last_child(size_t state);

        [[nodiscard]] bool operator<(const State &other) const;
    };

    /// Copies `current` into an internal buffer.
    [[nodiscard]] VoidResult set_previous(const BytesRef &current);
    size_t create_state();
    /// Internal recursive traversal for the conversion.
    [[nodiscard]] static Result<int32_t> convert(AutomatonBuilder &a, size_t state,
                                                 const std::vector<State> &all_states,
                                                 std::unordered_map<size_t, int32_t> &visited);
    /// Called after all terms were added: final minimization and conversion to
    /// a plain [`Automaton`].
    [[nodiscard]] Result<Automaton> complete_and_convert();
    [[nodiscard]] VoidResult add(const BytesRef &current, bool as_binary);
    size_t new_state(size_t state, int32_t label);
    /// Replaces the last child of `state` with an already registered state, or
    /// registers the last child state.
    [[nodiscard]] VoidResult replace_or_register(size_t state);

    /// A registry for state interning.
    std::map<State, size_t> state_registry_;
    /// All the automaton states; state references are indexes into this vector.
    std::vector<State> all_states_;
    /// The root automaton state.
    size_t root_{0};
    /// Used for input order checking.
    std::optional<BytesRefBuilder> previous_;
};

} // namespace lucene
