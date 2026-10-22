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

export module lucene_core:finite_strings_iterator;

import :types;
import std;

import :automaton;
import :error;
import :fixed_bit_set;
import :ints_ref;
import :transition;

namespace lucene {

/// Iterates all the accepted strings of an [`Automaton`].
///
/// If the automaton has cycles this iterator may report an error, but that is
/// not guaranteed.
///
/// Be aware that the iteration order is implementation dependent and may change
/// across releases.
///
/// If the automaton is not determinized, this iterator may return duplicates.
///
/// Port of `FiniteStringsIterator`
/// (`core/util/automation/finite_strings_iterator.rs`).
///
/// **Deviation:** the reference implementation returns `Cow<'_, IntsRef>`, a borrowed view of the
/// builder's buffer that is only valid until the next call.  The C++ port has no
/// borrowed `IntsRef` (the type always owns its storage), so `next` hands out a
/// copy in a `std::optional`; `nullopt` means "no more strings".
export class FiniteStringsIterator {
public:
    /// An iterator over the strings accepted from state `0`.
    explicit FiniteStringsIterator(const Automaton &a);
    /// An iterator over the paths from `start_state` to `end_state`; with
    /// `end_state == -1` only accept states end a path.
    FiniteStringsIterator(const Automaton &a, int32_t start_state, int32_t end_state);

    /// The next accepted string, or `nullopt` once iteration is finished.
    [[nodiscard]] Result<std::optional<IntsRef>> next();

private:
    /// One frame of the depth first walk.
    struct PathNode {
        /// The state whose transitions are being enumerated.
        int32_t state{0};
        /// The destination of the current transition.
        int32_t to{0};
        /// The index of the current transition.
        int32_t transition{0};
        /// The label inside the `[min, max]` range of the current transition.
        int32_t label{0};
        Transition t{};

        /// Starts enumerating the transitions leaving `state`.
        void reset_state(const Automaton &a, int32_t state);
        /// The next label of the current transition, or `-1` when the state has
        /// no more transitions.
        int32_t next_label(const Automaton &a);
    };

    /// Grows the path stack when a deeper level is needed.
    void grow_stack(size_t depth);

    const Automaton &a_;
    /// Where each path stops, or `-1` when only accept states end a path.
    int32_t end_state_{-1};
    /// The states on the current path, for cycle detection.
    FixedBitSet path_states_;
    /// The current string under construction.
    IntsRefBuilder string_;
    /// The current state of each level of the walk.
    std::vector<PathNode> nodes_;
    /// Whether the empty string still has to be emitted.
    bool emit_empty_string_{false};
};

} // namespace lucene
