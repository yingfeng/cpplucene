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

export module lucene_core:transition;

import :types;
import std;

namespace lucene {

/// Holds one transition from an automaton.  This is typically used temporarily
/// when iterating through transitions via
/// [`TransitionAccessor::init_transition`] and
/// [`TransitionAccessor::get_next_transition`].
///
/// Port of `Transition` (`core/util/automation/transition.rs`).
export struct Transition {
    /// Source state.
    int32_t source{0};
    /// Destination state.
    int32_t dest{0};
    /// Minimum accepted label (inclusive).
    int32_t min{0};
    /// Maximum accepted label (inclusive).
    int32_t max{0};
    /// Remembers where we are in the iteration; `nullopt` provokes an error if
    /// [`TransitionAccessor::get_next_transition`] is called without a preceding
    /// [`TransitionAccessor::init_transition`].
    std::optional<size_t> transition_upto{};

    [[nodiscard]] bool operator==(const Transition &other) const noexcept {
        return source == other.source && dest == other.dest && min == other.min && max == other.max &&
               transition_upto == other.transition_upto;
    }
};

} // namespace lucene
