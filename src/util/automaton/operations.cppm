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

export module lucene_core:operations;

import :types;
import std;

import :automaton;
import :bytes_ref;
import :error;
import :fixed_bit_set;
import :ints_ref;

namespace lucene {

/// Automata operations.
///
/// Port of `Operations` (`core/util/automation/operations.rs`).
///
/// **Deviation:** rlucene returns `Cow<'_, Automaton>`, so a function that does
/// not change anything hands the input back without copying.  The C++ port
/// returns `std::optional<Automaton>` instead: `nullopt` means "use the input
/// as is" (`Cow::Borrowed`), a value means "use this automaton"
/// (`Cow::Owned`).  Functions that can only borrow their first argument return
/// `nullopt` in that case; `intersection` therefore materialises the rarer
/// "borrow the second automaton" case (an automaton without states) instead of
/// returning `nullopt` for it.
///
/// **Deviation:** the rlucene lists of `&Automaton` (`&[A: Borrow<Automaton>]`)
/// are spelled `std::vector<const Automaton *>`.
export class Operations {
public:
    /// Default maximum effort that [`determinize`] should spend before giving
    /// up and returning `too_complex_to_determinize`.
    static constexpr size_t DEFAULT_DETERMINIZE_WORK_LIMIT = 10000;

    // ---- construction from inputs -------------------------------------------

    /// Returns an automaton that accepts the concatenation of the languages of
    /// the given automata.
    ///
    /// Complexity: linear in the total number of states.
    [[nodiscard]] static Result<Automaton> concatenate(const Automaton &a1, const Automaton &a2);
    /// [`concatenate`] over a list of automata, in order.
    [[nodiscard]] static Result<Automaton> concatenate_list(const std::vector<const Automaton *> &automata);

    /// Returns an automaton that accepts the union of the languages of the
    /// given automata.
    ///
    /// Complexity: linear in the number of states.
    [[nodiscard]] static Result<Automaton> union_with(const Automaton &a1, const Automaton &a2);
    /// [`union_with`] over a list of automata.
    [[nodiscard]] static Result<Automaton> union_list(const std::vector<const Automaton *> &automata);

    // ---- closures -----------------------------------------------------------

    /// Returns an automaton that accepts the union of the empty string and the
    /// language of the given automaton.  This may create a dead state.
    ///
    /// Complexity: linear in the number of states.
    [[nodiscard]] static Result<std::optional<Automaton>> optional(const Automaton &a);
    /// Returns an automaton that accepts the Kleene star (zero or more
    /// concatenated repetitions) of the language of the given automaton.
    /// Never modifies the input automaton language.
    ///
    /// Complexity: linear in the number of states.
    [[nodiscard]] static Result<std::optional<Automaton>> repeat(const Automaton &a);
    /// Returns an automaton that accepts `count` or more concatenated
    /// repetitions of the language of the given automaton.
    ///
    /// Complexity: linear in the number of states and in `count`.
    [[nodiscard]] static Result<std::optional<Automaton>> repeat_count(const Automaton &a, int32_t count);
    /// Returns an automaton that accepts between `min` and `max` (inclusive)
    /// concatenated repetitions of the language of the given automaton.
    ///
    /// Complexity: linear in the number of states, `min`, and `max`.
    [[nodiscard]] static Result<Automaton> repeat_min_max(const Automaton &a, int32_t min, int32_t max);

    // ---- boolean algebra ----------------------------------------------------

    /// Returns a (deterministic) automaton that accepts the complement of the
    /// language of the given automaton.
    ///
    /// Complexity: linear in the number of states if already deterministic, and
    /// exponential otherwise.
    [[nodiscard]] static Result<Automaton> complement(const Automaton &a, size_t determinize_work_limit);
    /// Returns a (deterministic) automaton that accepts the intersection of the
    /// language of `a1` and the complement of the language of `a2`.
    [[nodiscard]] static Result<std::optional<Automaton>> minus(const Automaton &a1, const Automaton &a2,
                                                               size_t determinize_work_limit);
    /// Returns an automaton that accepts the intersection of the languages of
    /// the given automata.  Never modifies the input automata languages.
    ///
    /// Complexity: quadratic in the number of states.
    [[nodiscard]] static Result<std::optional<Automaton>> intersection(const Automaton &a1, const Automaton &a2);

    // ---- inspection ---------------------------------------------------------

    /// `true` if this automaton has any states that cannot be reached from the
    /// initial state or cannot reach an accept state.
    ///
    /// Cost: O(num_transitions + num_states).
    [[nodiscard]] static Result<bool> has_dead_states(const Automaton &a);
    /// `true` if there are dead states reachable from an initial state.
    [[nodiscard]] static Result<bool> has_dead_states_from_initial(const Automaton &a);
    /// `true` if there are dead states that reach an accept state.
    [[nodiscard]] static Result<bool> has_dead_states_to_accept(const Automaton &a);
    /// `true` if the given automaton accepts no strings.
    [[nodiscard]] static Result<bool> is_empty(const Automaton &a);
    /// `true` if the given automaton accepts all Unicode strings.
    ///
    /// The automaton must be deterministic, otherwise this method may return
    /// `false`.
    [[nodiscard]] static Result<bool> is_total(const Automaton &a);
    /// `true` if the given automaton accepts every string over
    /// `[min_alphabet, max_alphabet]`.
    [[nodiscard]] static Result<bool> is_total_with_range(const Automaton &a, int32_t min_alphabet, int32_t max_alphabet);

    /// `true` if the given string is accepted by the automaton.  The automaton
    /// must be deterministic.
    [[nodiscard]] static bool run_str(const Automaton &a, std::string_view s);
    /// `true` if the given codepoints are accepted by the automaton.  The
    /// automaton must be deterministic.
    [[nodiscard]] static bool run_ints_ref(const Automaton &a, const IntsRef &s);

    // ---- live states --------------------------------------------------------

    /// The set of live states: reachable from the initial state and able to
    /// reach an accept state.
    [[nodiscard]] static Result<FixedBitSet> get_live_states(const Automaton &a);
    /// The states reachable from the initial state.
    [[nodiscard]] static Result<FixedBitSet> get_live_states_from_initial(const Automaton &a);
    /// The states that can reach an accept state.
    [[nodiscard]] static Result<FixedBitSet> get_live_states_to_accept(const Automaton &a);

    /// Removes transitions to dead states; `nullopt` when nothing was removed.
    [[nodiscard]] static Result<std::optional<Automaton>> remove_dead_states(const Automaton &a);

    /// Adds transitions to a dead state so that from every state and every
    /// label there is a transition.
    [[nodiscard]] static Result<Automaton> totalize(const Automaton &a);

    // ---- common prefixes and suffixes ---------------------------------------

    /// The longest string that is a prefix of all accepted strings and visits
    /// each state at most once.  Fails when the automaton has dead states
    /// reachable from the initial state.
    [[nodiscard]] static Result<std::string> get_common_prefix(const Automaton &a);
    /// [`get_common_prefix`] as bytes; fails when the prefix is not binary.
    [[nodiscard]] static Result<BytesRef> get_common_prefix_bytes_ref(const Automaton &a);
    /// The longest string that is a suffix of all accepted strings.  May
    /// include a UTF-8 fragment.
    [[nodiscard]] static Result<BytesRef> get_common_suffix_bytes_ref(const Automaton &a);

    /// The single accepted string, or `nullopt` when the automaton accepts
    /// zero or more than one string.  The automaton must be deterministic.
    [[nodiscard]] static Result<std::optional<IntsRef>> get_singleton(const Automaton &a);

    // ---- reversal -----------------------------------------------------------

    /// Returns an automaton accepting the reverse language.
    [[nodiscard]] static Result<Automaton> reverse(const Automaton &a);
    /// [`reverse`], additionally reporting the new initial states.
    [[nodiscard]] static Result<Automaton> reverse_with_initial_states(const Automaton &a,
                                                                      std::set<int32_t> *initial_states);

    // ---- ordering -----------------------------------------------------------

    /// Topological sort of all states reachable from the initial state, or an
    /// error when the automaton contains a cycle.
    [[nodiscard]] static Result<std::vector<int32_t>> topo_sort_states(const Automaton &a);
    /// [`topo_sort_states`] into a caller supplied buffer; the number of
    /// states written is returned.
    [[nodiscard]] static Result<size_t> topo_sort_states_with_state(const Automaton &a, std::span<int32_t> states);

    // ---- determinization ----------------------------------------------------

    /// Determinizes the given automaton; `nullopt` when it already is
    /// deterministic.
    ///
    /// Worst case complexity: exponential in the number of states.  Fails with
    /// `too_complex_to_determinize` when the work exceeds `work_limit`
    /// (rlucene's effort limit is `work_limit * 10`).
    [[nodiscard]] static Result<std::optional<Automaton>> determinize(const Automaton &a, size_t work_limit);

private:
    /// Fills `states` with the accept states of `a` offset by `offset`.
    [[nodiscard]] static std::set<int32_t> get_accept_states(const Automaton &a, int32_t offset);
};

} // namespace lucene
