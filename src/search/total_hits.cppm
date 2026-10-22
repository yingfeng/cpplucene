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

export module lucene_core:total_hits;

import :types;

import std;

namespace lucene {

/// Description of the total number of hits of a query.
///
/// Port of `TotalHits` (`core/search/total_hits.rs`).
///
/// Visiting every match is expensive, and callers often only need a lower
/// bound such as "more than 1000 hits", which is why the count can stop early
/// (see `TopScoreDocCollector`'s total-hits threshold).
///
/// **Deviation:** the reference implementation's `Relation` is a top-level enum.  This port already
/// has a top-level `Relation` for point queries (`:point_values`), so the hit
/// relation is nested here, the way Java nests `TotalHits.Relation`.
export struct TotalHits {
    /// How [`value`] should be interpreted.
    enum class Relation : int32_t {
        /// `value` is the exact hit count.
        EqualTo = 0,
        /// `value` is a lower bound of the hit count.
        GreaterThanOrEqualTo = 1,
    };

    /// The hit count, to be read together with [`relation`].
    size_t value{0};
    /// Whether [`value`] is exact or a lower bound.
    Relation relation{Relation::EqualTo};

    [[nodiscard]] static TotalHits create(size_t value, Relation relation) { return TotalHits{value, relation}; }

    /// `"<n> hits"` or `"<n>+ hits"`, for diagnostics.
    [[nodiscard]] std::string to_string() const {
        return relation == Relation::EqualTo ? std::to_string(value) + " hits" : std::to_string(value) + "+ hits";
    }

    friend bool operator==(const TotalHits &, const TotalHits &) = default;
};

} // namespace lucene
