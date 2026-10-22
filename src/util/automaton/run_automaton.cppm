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

export module lucene_core:run_automaton;

import :types;
import std;

import :automaton;
import :error;
import :fixed_bit_set;
import :transition;

namespace lucene {

/// Finite-state automaton with a fast run operation; the initial state is
/// always 0.
///
/// Port of `RunAutomaton` (`core/util/automation/run_automaton.rs`).
///
/// The automaton is flattened into a table indexed by
/// `state * points.size() + get_char_class(c)`, where `points` are the
/// codepoint class interval start points.
///
/// **Deviations:** the reference implementation returns `Result<Self>` from `new`, which a C++
/// constructor cannot, so [`create`] is the factory; the `Accountable`
/// implementation is not ported.
export class RunAutomaton {
public:
    /// Builds the run tables of `automaton` over an alphabet of
    /// `alphabet_size` codepoints; fails when `automaton` is not deterministic.
    [[nodiscard]] static Result<RunAutomaton> create(Automaton automaton, size_t alphabet_size);

    /// Number of states.
    [[nodiscard]] size_t size() const noexcept { return size_; }
    /// Whether `state` is accepting; `state` must be in range.
    [[nodiscard]] Result<bool> is_accept(int32_t state) const;
    /// Codepoint class interval start points; owned by this automaton.
    [[nodiscard]] const std::vector<int32_t> &char_intervals() const noexcept { return points_; }
    /// The codepoint class of `c`.
    [[nodiscard]] size_t get_char_class(int32_t c) const noexcept;
    /// The state reached by reading `c` in `state`, or `-1`.
    [[nodiscard]] Result<int32_t> step(int32_t state, int32_t c) const;

    /// The flattened automaton this was built from.
    [[nodiscard]] const Automaton &automaton() const noexcept { return automaton_; }

    [[nodiscard]] bool operator==(const RunAutomaton &other) const noexcept {
        return alphabet_size_ == other.alphabet_size_ && size_ == other.size_ && points_ == other.points_ &&
               transitions_ == other.transitions_ && accept_.words() == other.accept_.words();
    }

    /// Human readable dump of the flattened tables, for debugging.
    [[nodiscard]] std::string to_dot() const;

private:
    RunAutomaton() = default;

    Automaton automaton_{};
    size_t alphabet_size_{0};
    size_t size_{0};
    FixedBitSet accept_{};
    /// `transitions_[state * points_.size() + class]`, `-1` when no transition.
    std::vector<int32_t> transitions_{};
    std::vector<int32_t> points_{};
    std::vector<size_t> classmap_{};
};

} // namespace lucene
