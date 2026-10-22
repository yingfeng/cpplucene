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

export module lucene_core:conjunction_scorer;

import :types;

import std;

import :conjunction_disi;
import :doc_id_set_iterator;
import :error;
import :scorer;

namespace lucene {

/// A scorer over the intersection of several sub-scorers, summing their scores.
///
/// Port of `ConjunctionScorer` (`core/search/conjunction_scorer.rs`).
///
/// The document set comes from a [`ConjunctionDISI`] over the sub-scorers, and
/// only the `scoring` sub-scorers contribute to the score — a `FILTER` clause
/// intersects without scoring, which is the `scoring_idx` list of rlucene.
///
/// **Deviations** (C++ mechanics only):
///  * rlucene selects between plain, two-phase and block-max implementations at
///    construction (`ConjunctionScorerDisi`); the port always builds the plain
///    conjunction and lets the two-phase confirmation run afterwards, which is
///    slower but exact (block-max needs impacts, which are not ported);
///  * the sub-scorers are owned; a single-clause conjunction has to be unwrapped
///    by the caller ([`ConjunctionUtils::intersect_scorers`](lucene_core:conjunction_utils)
///    does that).
export class ConjunctionScorer final : public Scorer {
public:
    /// `required` must hold at least two scorers, and every index in `scoring`
    /// must be in range.
    [[nodiscard]] static Result<std::unique_ptr<ConjunctionScorer>> create(
            std::vector<std::unique_ptr<Scorer>> required, std::vector<size_t> scoring);

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

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

    /// The sub-scorers, in input order.
    [[nodiscard]] const std::vector<std::unique_ptr<Scorer>> &sub_scorers() const noexcept { return required_; }

private:
    ConjunctionScorer(std::vector<std::unique_ptr<Scorer>> required, std::vector<size_t> scoring);

    std::vector<std::unique_ptr<Scorer>> required_;
    std::vector<size_t> scoring_;
    std::unique_ptr<ConjunctionDISI> disi_;
};

} // namespace lucene
