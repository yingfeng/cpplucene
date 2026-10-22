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

export module lucene_core:state_pair;

import :types;
import std;

namespace lucene {

/// A pair of states, used as a work list entry while exploring the product of
/// two automata.
///
/// Port of `StatePair` (`core/util/automation/state_pair.rs`).
export class StatePair {
public:
    /// A pair of source states; `s` is left unset.
    static StatePair create(int32_t s1, int32_t s2) noexcept { return StatePair{s1, s2, -1}; }
    /// A pair of source states that already maps to the product state `s`.
    static StatePair with_s(int32_t s, int32_t s1, int32_t s2) noexcept { return StatePair{s1, s2, s}; }

    /// Only the source states take part in equality.
    [[nodiscard]] bool operator==(const StatePair &other) const noexcept { return s1 == other.s1 && s2 == other.s2; }

    /// First source state.
    int32_t s1{-1};
    /// Second source state.
    int32_t s2{-1};
    /// The product state, when it has been allocated.
    int32_t s{-1};
};

/// Hash of a [`StatePair`], over the source states only.
export struct StatePairHash {
    [[nodiscard]] size_t operator()(const StatePair &pair) const noexcept {
        return std::hash<int32_t>{}(pair.s1) * 31u + std::hash<int32_t>{}(pair.s2);
    }
};

} // namespace lucene
