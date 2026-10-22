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

export module lucene_core:point_transition_set;

import :types;
import std;

import :transition;

namespace lucene {

/// A list of `{dest, min, max}` triples, packed three integers per transition.
///
/// Port of `TransitionList` (`core/util/automation/operations.rs`).
export struct TransitionList {
    std::vector<int32_t> transitions{};
    size_t next{0};

    void add(const Transition &t) {
        if (transitions.size() < next + 3) {
            transitions.resize(next + 3, 0);
        }
        transitions[next] = t.dest;
        transitions[next + 1] = t.min;
        transitions[next + 2] = t.max;
        next += 3;
    }
};

/// All the transitions starting or ending at one point, used to walk the
/// interval boundaries of a state set.
///
/// Port of `PointTransitions` (`core/util/automation/operations.rs`).
export struct PointTransitions {
    int32_t point{0};
    TransitionList ends{};
    TransitionList starts{};

    void reset(int32_t reset_point) {
        point = reset_point;
        ends.next = 0;
        starts.next = 0;
    }
};

/// The interval boundaries of a set of transitions, sorted on demand.
///
/// Port of `PointTransitionSet` (`core/util/automation/operations.rs`); the
/// lookups switch to a hash map once the set grows past
/// `HASHMAP_CUTOVER` points.
export class PointTransitionSet {
public:
    /// The number of points in the set.
    size_t count{0};
    /// The points, only the first `count` of them being live.
    std::vector<PointTransitions> points{};

    /// The entry for `point`, adding it when it is new.
    PointTransitions &find(int32_t point);

    /// Removes every point.
    void reset();

    /// Sorts the live points by their label.
    void sort();

    /// Adds a transition, extending its interval when it is already present.
    void add(const Transition &t);

private:
    static constexpr size_t HASHMAP_CUTOVER = 30;

    /// The index of a new entry for `point`.
    size_t next(int32_t point);

    std::unordered_map<int32_t, size_t> map_{};
    bool use_hash_{false};
};

} // namespace lucene
