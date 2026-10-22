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

export module lucene_core:minimum_should_match_scorer;

import :types;

import std;

import :disjunction_disi_approximation;
import :doc_id_set_iterator;
import :error;
import :scorer;

namespace lucene {

/// A disjunction where at least `min_should_match` clauses have to match.
///
/// Port of the non-block-max part of `WANDScorer`
/// (`core/search/wand_scorer.rs`), which is what a `BooleanQuery` with
/// `minimum_should_match > 1` builds.
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation implements the WAND skipping algorithm over impacts; the port has
///    no impacts, so this is a plain disjunction iteration that rejects
///    candidates which confirm fewer clauses than required.  Results are the
///    same, only slower;
///  * the check is done in the two-phase confirmation pass
///    ([`matches`]) instead of inside the iterator, which is why
///    `has_two_phase_iterator()` reports `Yes`.
export class MinimumShouldMatchScorer final : public Scorer {
public:
    /// `sub_scorers` must hold at least two scorers, and `min_should_match` must
    /// be in `[1, sub_scorers.size()]`.
    [[nodiscard]] static Result<std::unique_ptr<MinimumShouldMatchScorer>>
    create(std::vector<std::unique_ptr<Scorer>> sub_scorers, int32_t min_should_match);

    // ---- DocIdSetIterator: the plain disjunction ----

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    // ---- Scorer ----

    [[nodiscard]] Result<int32_t> scoring_doc_id() override;
    [[nodiscard]] Result<float> get_max_score(int32_t upto) override;
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::Yes; }
    [[nodiscard]] Result<bool> matches() override;
    [[nodiscard]] float match_cost() const override;
    [[nodiscard]] int32_t approx_doc_id() const override;
    [[nodiscard]] Result<int32_t> approx_next_doc() override;
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> approx_cost() const override;
    [[nodiscard]] Result<float> score() override;
    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override;

    /// How many clauses match the current document.
    [[nodiscard]] int32_t min_should_match() const noexcept { return min_should_match_; }

private:
    MinimumShouldMatchScorer(std::vector<std::unique_ptr<Scorer>> sub_scorers,
                             std::unique_ptr<DisjunctionDISIApproximation> approximation, int32_t min_should_match)
        : sub_scorers_(std::move(sub_scorers)), approximation_(std::move(approximation)),
          min_should_match_(min_should_match) {}

    std::vector<std::unique_ptr<Scorer>> sub_scorers_;
    std::unique_ptr<DisjunctionDISIApproximation> approximation_;
    int32_t min_should_match_;
};

} // namespace lucene
