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

export module lucene_core:req_excl_scorer;

import :types;

import std;

import :doc_id_set_iterator;
import :error;
import :scorer;

namespace lucene {

/// A `MUST` clause minus a `MUST_NOT` clause.
///
/// Port of `ReqExclScorer` (`core/search/req_excl_scorer.rs`).
///
/// The document set is the required clause's approximation; confirmation
/// ([`matches`]) advances the excluded clause to the same document and rejects
/// it.  Which confirmation runs first (`check_req`) is chosen at construction
/// from the approximated match costs, exactly like the reference implementation's two
/// `TwoPhaseIteratorImpl`s.
///
/// **Deviation:** the reference implementation wraps the two impls in `TwoPhaseIteratorEnum2` and
/// keeps a `TwoPhaseIteratorAsDocIdSetIterator`; the port folds both into one
/// class plus a `check_req_` flag, because the two only differ in the order of
/// the `matches` calls.
export class ReqExclScorer final : public Scorer {
public:
    [[nodiscard]] static Result<std::unique_ptr<ReqExclScorer>> create(std::unique_ptr<Scorer> required,
                                                                      std::unique_ptr<Scorer> excluded);

    // ---- DocIdSetIterator: the required clause's approximation ----

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
    ReqExclScorer(std::unique_ptr<Scorer> required, std::unique_ptr<Scorer> excluded, float match_cost,
                  bool check_req)
        : required_(std::move(required)), excluded_(std::move(excluded)), match_cost_(match_cost),
          check_req_(check_req) {}

    std::unique_ptr<Scorer> required_;
    std::unique_ptr<Scorer> excluded_;
    float match_cost_;
    bool check_req_;
};

} // namespace lucene
