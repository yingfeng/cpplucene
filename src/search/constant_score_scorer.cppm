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

export module lucene_core:constant_score_scorer;

import :types;
import std;

import :doc_id_set_iterator;
import :error;
import :score_mode;
import :scorable;
import :scorer;

namespace lucene {

/// A scorer that gives every document it iterates the same score.
///
/// Port of `ConstantScoreDisiScorer` (`core/search/constant_score_scorer.rs`),
/// reduced to the plain `DocIdSetIterator` form (rlucene also has a two-phase
/// approximation form and a bit-set form).
///
/// **Deviation:** the wrapped iterator is owned; rlucene's enum keeps the
/// variant inline.  Score modes that do not need scores still report the score,
/// since nothing prunes with it in this port yet.
export class ConstantScoreScorer final : public Scorer {
public:
    /// [`EmptyDISI`] for the "this leaf cannot match" case of rlucene's
    /// `empty_scorer`.
    [[nodiscard]] static std::unique_ptr<ConstantScoreScorer> from_disi(float score, ScoreMode score_mode,
                                                                        std::unique_ptr<DocIdSetIterator> iterator);

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_id(); }

    [[nodiscard]] Result<float> get_max_score(int32_t upto) override;

    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }

    [[nodiscard]] Result<float> score() override { return score_; }

private:
    ConstantScoreScorer(float score, ScoreMode score_mode, std::unique_ptr<DocIdSetIterator> iterator)
            : score_(score), score_mode_(score_mode), iterator_(std::move(iterator)) {}

    float score_;
    ScoreMode score_mode_;
    std::unique_ptr<DocIdSetIterator> iterator_;
};

} // namespace lucene
