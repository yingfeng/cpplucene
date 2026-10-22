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

export module lucene_core:disjunction_scorer;

import :types;

import std;

import :disjunction_disi_approximation;
import :doc_id_set_iterator;
import :error;
import :scorer;

namespace lucene {

/// A scorer over the union of several sub-scorers, summing the scores of the
/// ones that match the current document.
///
/// Ports `DisjunctionSumScorer` + the plain `DisjunctionScorer`
/// (`core/search/disjunction_sum_scorer.rs`, `core/search/disjunction_scorer.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation's `DisjunctionScorer<D, T>` is generic over the iterator and the
///    score collector; the port has the one implementation the boolean query
///    needs (`DisjunctionSumScorer`) and keeps the iterator separate;
///  * `DisjunctionMatchesIterator` is not ported: [`matches`] walks the
///    sub-scorers that sit on the current document instead, which gives the same
///    answer (a disjunction matches when at least one *confirmed* sub-scorer
///    matches) without building a multi-term iterator;
///  * block-max scoring (`BlockMaxDisjunctionScorer`) needs impacts and is not
///    ported.
export class DisjunctionSumScorer final : public Scorer {
public:
    /// `sub_scorers` must hold at least two scorers.
    [[nodiscard]] static Result<std::unique_ptr<DisjunctionSumScorer>>
    create(std::vector<std::unique_ptr<Scorer>> sub_scorers);

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

    /// The number of sub-scorers currently on the current document.
    [[nodiscard]] Result<int32_t> freq();

private:
    DisjunctionSumScorer(std::vector<std::unique_ptr<Scorer>> sub_scorers,
                         std::unique_ptr<DisjunctionDISIApproximation> disi)
        : sub_scorers_(std::move(sub_scorers)), disi_(std::move(disi)) {}

    std::vector<std::unique_ptr<Scorer>> sub_scorers_;
    std::unique_ptr<DisjunctionDISIApproximation> disi_;
};

} // namespace lucene
