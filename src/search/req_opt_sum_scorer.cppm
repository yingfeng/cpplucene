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

export module lucene_core:req_opt_sum_scorer;

import :types;

import std;

import :doc_id_set_iterator;
import :error;
import :scorer;

namespace lucene {

/// A `MUST` clause combined with a `SHOULD` clause: the documents are the
/// required ones, the score is the sum of both when the optional clause matches
/// too.
///
/// Port of `ReqOptSumScorer` (`core/search/req_opt_sum_scorer.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * rlucene keeps a `min_score` and a required-clause block upper bound so that
///    it can turn the optional clause into a second required one
///    (`advance_impacts`/`move_to_next_block`); those need impacts and
///    `advance_shallow`, which the port does not have, so the scorer implements
///    the plain "advance the required clause, add the optional score"
///    algorithm.  It is exact, just without the skipping optimisation;
///  * `set_min_competitive_score` is still forwarded to both children, which is
///    harmless and keeps later impact support possible.
export class ReqOptSumScorer final : public Scorer {
public:
    [[nodiscard]] static Result<std::unique_ptr<ReqOptSumScorer>> create(std::unique_ptr<Scorer> required,
                                                                        std::unique_ptr<Scorer> optional);

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

private:
    ReqOptSumScorer(std::unique_ptr<Scorer> required, std::unique_ptr<Scorer> optional)
        : required_(std::move(required)), optional_(std::move(optional)) {}

    std::unique_ptr<Scorer> required_;
    std::unique_ptr<Scorer> optional_;
};

} // namespace lucene
