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

export module lucene_core:character_run_automaton;

import :types;
import std;

import :automaton;
import :error;
import :run_automaton;

namespace lucene {

/// Automaton representation for matching decoded character sequences, i.e. over
/// the full Unicode codepoint alphabet.
///
/// Port of `CharacterRunAutomaton`
/// (`core/util/automation/character_run_automaton.rs`).
export class CharacterRunAutomaton {
public:
    /// Builds the run tables; fails when `automaton` is not deterministic.
    [[nodiscard]] static Result<CharacterRunAutomaton> create(Automaton automaton);

    /// Whether `s` is accepted.
    [[nodiscard]] Result<bool> run_str(std::string_view s) const;
    /// Whether the UTF-16 slice `chars[offset, offset + length)` is accepted;
    /// malformed surrogate pairs are rejected.
    [[nodiscard]] Result<bool> run_chars(std::span<const uint16_t> chars, size_t offset, size_t length) const;

    /// The underlying table driven automaton.
    [[nodiscard]] const RunAutomaton &base() const noexcept { return base_; }

    [[nodiscard]] bool operator==(const CharacterRunAutomaton &other) const noexcept { return base_ == other.base_; }

private:
    explicit CharacterRunAutomaton(RunAutomaton base) : base_(std::move(base)) {}

    RunAutomaton base_;
};

} // namespace lucene
