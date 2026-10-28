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

export module lucene_core:disjunction_max_scorer;

import :types;
import std;

import :disjunction_score_block_boundary_propagator;
import :doc_id_set_iterator;
import :error;
import :score_mode;
import :scorer;
import :two_phase_iterator;

namespace lucene {

/// A scorer over the union of several sub-scorers that takes the maximum score
/// per document and adds `tie_breaker_multiplier` times the sum of the other
/// matching sub-scores.
///
/// Port of `DisjunctionMaxScorer` (`core/search/disjunction_max_scorer.rs`).
///
/// **Deviations:**
///  * the reference implementation's `DisjunctionScorerBase` (a min-heap over
///    `DisiWrapper`s with per-sub links) is not ported as a class: the scorer
///    keeps the same shape as [`DisjunctionSumScorer`](lucene_core:disjunction_scorer),
///    scoring by walking the sub-scorers that sit on the current document,
///    which is equivalent and much simpler in C++;
///  * the block boundary propagator
///    ([`DisjunctionScoreBlockBoundaryPropagator`](lucene_core:disjunction_score_block_boundary_propagator))
///    is built when the score mode is `TopScores`, which is when a minimum
///    competitive score can prune whole blocks; with another mode
///    `advance_shallow` keeps the base implementation that reports
///    `NO_MORE_DOCS`.
export class DisjunctionMaxScorer final : public Scorer {
public:
    /// `sub_scorers` must hold at least two scorers; `tie_breaker_multiplier`
    /// has to be in `[0, 1]`.
    [[nodiscard]] static Result<std::unique_ptr<DisjunctionMaxScorer>>
    create(float tie_breaker_multiplier, std::vector<std::unique_ptr<Scorer>> sub_scorers, ScoreMode score_mode);

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    /// The last document of the block that the clauses which can still compete
    /// share, so a caller may skip straight to it.
    [[nodiscard]] Result<int32_t> advance_shallow(int32_t target) override;

    // ---- Scorer ----

    [[nodiscard]] Result<int32_t> scoring_doc_id() override;
    [[nodiscard]] Result<float> get_max_score(int32_t upto) override;
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override;
    [[nodiscard]] Result<bool> matches() override;
    [[nodiscard]] float match_cost() const override;
    [[nodiscard]] int32_t approx_doc_id() const override;
    [[nodiscard]] Result<int32_t> approx_next_doc() override;
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> approx_cost() const override;
    [[nodiscard]] Result<float> score() override;
    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override;

    /// The tie breaker multiplier the scorer was created with.
    [[nodiscard]] float tie_breaker_multiplier() const noexcept { return tie_breaker_multiplier_; }

private:
    DisjunctionMaxScorer(std::vector<std::unique_ptr<Scorer>> sub_scorers, std::vector<Scorer *> raw_scorers,
                         std::unique_ptr<DocIdSetIterator> disi, float tie_breaker_multiplier, ScoreMode score_mode)
            : sub_scorers_(std::move(sub_scorers)),
              raw_scorers_(std::move(raw_scorers)),
              disi_(std::move(disi)),
              tie_breaker_multiplier_(tie_breaker_multiplier),
              score_mode_(score_mode) {}

    std::vector<std::unique_ptr<Scorer>> sub_scorers_;
    /// The same scorers, in the order the propagator and the approximation want.
    std::vector<Scorer *> raw_scorers_;
    std::unique_ptr<DocIdSetIterator> disi_;
    float tie_breaker_multiplier_;
    ScoreMode score_mode_;
    /// Set when the score mode is `TopScores`; it answers [`advance_shallow`].
    std::optional<DisjunctionScoreBlockBoundaryPropagator> propagator_;
};

} // namespace lucene
