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

export module lucene_core:disjunction_score_block_boundary_propagator;

import :types;
import std;

import :error;
import :scorer;

namespace lucene {

/// Propagates block boundaries for a disjunction.
///
/// Port of `DisjunctionScoreBlockBoundaryPropagator`
/// (`core/search/disjunction_score_block_boundary_propagator.rs`).
///
/// A disjunction matches whenever one of its clauses matches, so it is tempting
/// to answer the minimum block boundary across all clauses.  That is wrong once
/// the minimum competitive score is high: low-scoring clauses would keep driving
/// the iteration while they can no longer contribute a competitive hit.  This
/// helper therefore only considers the clauses whose maximum score is at least
/// the minimum competitive score, plus the maximum-scoring clause.
///
/// **Deviation:** the reference implementation reaches the scorers through the
/// `DisiWrapper`s of the owning disjunction; here the callers pass the scorer
/// pointers, which keeps the helper independent of the wrapper shape.
export class DisjunctionScoreBlockBoundaryPropagator {
public:
    /// The maximum score and the iteration cost of one clause.
    struct Cost {
        float max_score{0.0F};
        size_t idx{0};
        int64_t iter_cost{0};
    };

    /// Reads the maximum score of every scorer, shallow-advancing it to `0`
    /// first, and orders them by score then by cost.
    [[nodiscard]] static Result<DisjunctionScoreBlockBoundaryPropagator> create(std::span<Scorer *const> scorers);

    /// Shallow-advances the scorers to `target` and answers the last document of
    /// the block that is common to the clauses that can still compete.
    [[nodiscard]] Result<int32_t> advance_shallow(int32_t target, std::span<Scorer *const> scorers);

    /// Raises the minimum competitive score, dropping clauses that can no longer
    /// compete from the boundary computation.
    void set_min_competitive_score(float min_score);

    /// The clauses, ordered by maximum score then by cost.
    [[nodiscard]] const std::vector<Cost> &costs() const noexcept { return cost_; }

    /// The index of the first clause that can still compete.
    [[nodiscard]] size_t lead_index() const noexcept { return lead_index_; }

private:
    std::vector<Cost> cost_;
    size_t lead_index_{0};
};

} // namespace lucene
