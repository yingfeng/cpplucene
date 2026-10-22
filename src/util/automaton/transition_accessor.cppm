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

export module lucene_core:transition_accessor;

import :types;
import std;

import :error;
import :transition;

namespace lucene {

/// Access to the transitions of an automaton.
///
/// Port of the `TransitionAccessor` trait
/// (`core/util/automation/transition_accessor.rs`).
export class TransitionAccessor {
public:
    virtual ~TransitionAccessor() = default;

    /// Initializes `t` to iterate through all transitions leaving `state`, and
    /// returns how many transitions leave it.
    [[nodiscard]] virtual int32_t init_transition(int32_t state, Transition &t) const = 0;

    /// Advances `t` to the next transition outgoing from its source state.
    [[nodiscard]] virtual VoidResult get_next_transition(Transition &t) const = 0;

    /// How many transitions this state has.
    [[nodiscard]] virtual int32_t get_num_transitions_with_state(int32_t state) const = 0;

    /// Fills `t` with the `index`-th transition leaving `state`.
    virtual void get_transition(int32_t state, int32_t index, Transition &t) const = 0;
};

} // namespace lucene
