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

module lucene_core:point_transition_set.impl;

import :point_transition_set;

import :debug;
import :transition;
import :types;

import std;

namespace lucene {

PointTransitions &PointTransitionSet::find(int32_t point) {
    if (use_hash_) {
        const auto found = map_.find(point);
        if (found != map_.end()) {
            return points[found->second];
        }
        const size_t index = next(point);
        map_.emplace(point, index);
        return points[index];
    }

    for (size_t i = 0; i < count; ++i) {
        if (points[i].point == point) {
            return points[i];
        }
    }

    const size_t index = next(point);
    if (count == HASHMAP_CUTOVER) {
        debug_check(map_.empty(), "the hash is populated only once");
        map_.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            map_.emplace(points[i].point, i);
        }
        use_hash_ = true;
    }
    return points[index];
}

void PointTransitionSet::reset() {
    if (use_hash_) {
        map_.clear();
        use_hash_ = false;
    }
    count = 0;
}

void PointTransitionSet::sort() {
    if (count > 1) {
        std::sort(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(count),
                  [](const PointTransitions &left, const PointTransitions &right) { return left.point < right.point; });
    }
}

void PointTransitionSet::add(const Transition &t) {
    find(t.min).starts.add(t);
    find(t.max + 1).ends.add(t);
}

size_t PointTransitionSet::next(int32_t point) {
    if (count == points.size()) {
        points.emplace_back();
    }
    points[count].reset(point);
    count += 1;
    return count - 1;
}

} // namespace lucene
