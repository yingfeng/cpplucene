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

export module lucene_core:block_max_conjunction_scorer;

import :types;
import std;

import :doc_id_set_iterator;
import :error;
import :scorable;
import :scorer;
import :similarity;

namespace lucene {

/// Scorer for conjunctions that checks the maximum scores of each clause in
/// order to potentially skip over blocks that cannot have competitive matches.
///
/// Port of `BlockMaxConjunctionScorer`
/// (`core/search/block_max_conjunction_scorer.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation's `DocIdSetIteratorImpl` +
///    `TwoPhaseIteratorImpl` + `TwoPhaseIteratorAsDocIdSetIterator` triple
///    collapses into this one class, which reports two-phase iteration through
///    [`has_two_phase_iterator`] and confirms it in [`matches`];
///  * the bit-set accessors (`ApproximationBitSetAccess`) are not ported, as the
///    wrapped scorers do not expose them here.
export class BlockMaxConjunctionScorer final : public Scorer {
public:
    /// Sorts `scorers` by cost (the cheapest leads, equal costs keep their
    /// order), shallow-advances them to `0` and sums the match costs of the
    /// clauses that need two-phase confirmation.
    [[nodiscard]] static Result<std::unique_ptr<BlockMaxConjunctionScorer>>
    prepare(std::vector<std::unique_ptr<Scorer>> scorers);

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    // ---- Scorer ----

    [[nodiscard]] Result<int32_t> scoring_doc_id() override;
    [[nodiscard]] Result<int32_t> advance_shallow(int32_t target) override;
    [[nodiscard]] Result<float> get_max_score(int32_t upto) override;

    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override {
        return two_phase_ ? TwoPhaseState::Yes : TwoPhaseState::No;
    }

    [[nodiscard]] Result<bool> matches() override;
    [[nodiscard]] float match_cost() const override { return match_cost_; }

    // ---- Scorable ----

    /// The sum of the scores of the clauses.
    [[nodiscard]] Result<float> score() override;

    /// Raises the threshold that decides whether a block can still compete.
    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override;

    [[nodiscard]] Result<std::vector<ChildScorable>> get_children() override;

    /// The clauses, the cheapest first.
    [[nodiscard]] const std::vector<std::unique_ptr<Scorer>> &scorers() const noexcept { return scorers_; }

    /// The current block boundary, `-1` before the first block is read.
    [[nodiscard]] int32_t upto() const noexcept { return upto_; }

private:
    BlockMaxConjunctionScorer(std::vector<std::unique_ptr<Scorer>> scorers,
                              std::vector<std::pair<size_t, float>> two_phase_children, float match_cost);

    /// Moves the lead scorer forward and returns the next document on which every
    /// clause agrees.
    [[nodiscard]] Result<int32_t> do_next(int32_t doc);

    /// Moves to the block that contains `target`, either disabling pruning (no
    /// threshold yet) or reading the block boundary and maximum score.
    [[nodiscard]] VoidResult move_to_next_block(int32_t target);

    /// Answers `target`, or the first document of a later block that can still
    /// reach the minimum competitive score.
    [[nodiscard]] Result<int32_t> advance_target(int32_t target);

    [[nodiscard]] Result<int32_t> scorer_doc_id();

    std::vector<std::unique_ptr<Scorer>> scorers_;
    /// The clauses that need two-phase confirmation, the cheapest first.
    std::vector<std::pair<size_t, float>> two_phase_children_;
    float match_cost_{0.0F};
    bool two_phase_{false};
    int32_t upto_{-1};
    float max_score_{0.0F};
    float min_score_{0.0F};
};

} // namespace lucene
