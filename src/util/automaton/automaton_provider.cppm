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

export module lucene_core:automaton_provider;

import :types;
import std;

import :automaton;

namespace lucene {

/// Provider of named automata for [`RegExp::to_automaton_from_provider`].
///
/// Port of the `AutomatonProvider` trait
/// (`core/util/automation/automaton_provider.rs`).
export class AutomatonProvider {
public:
    AutomatonProvider() = default;
    AutomatonProvider(const AutomatonProvider &) = default;
    AutomatonProvider &operator=(const AutomatonProvider &) = default;
    AutomatonProvider(AutomatonProvider &&) = default;
    AutomatonProvider &operator=(AutomatonProvider &&) = default;
    virtual ~AutomatonProvider() = default;

    /// The automaton registered under `name`, or `nullopt` when there is none.
    [[nodiscard]] virtual Result<std::optional<Automaton>> get_automaton(std::string_view name) const = 0;
};

/// An [`AutomatonProvider`] that knows no automata at all.
export class DefaultProvider final : public AutomatonProvider {
public:
    [[nodiscard]] Result<std::optional<Automaton>> get_automaton(std::string_view name) const override;
};

/// An [`AutomatonProvider`] backed by a map; the `AutomatonMap` that the reference implementation
/// passes to `RegExp::to_automaton_from_map` takes the same role.
export class AutomatonMapProvider final : public AutomatonProvider {
public:
    explicit AutomatonMapProvider(const std::map<std::string, Automaton> &automata) : automata_(automata) {}

    [[nodiscard]] Result<std::optional<Automaton>> get_automaton(std::string_view name) const override;

private:
    const std::map<std::string, Automaton> &automata_;
};

} // namespace lucene
