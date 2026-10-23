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

export module lucene_core:phrase_scorer;

import :types;
import std;

import :error;
import :numeric_doc_values;
import :phrase_matcher;
import :scorable;
import :score_mode;
import :scorer;
import :similarity;
import :two_phase_iterator;

namespace lucene {

/// Scores the documents a [`PhraseMatcher`] finds, by the phrase frequency the
/// matcher accumulates and the field norm.
///
/// Port of `PhraseScorer` (`core/search/phrase_scorer.rs`).
///
/// **Deviations** (C++ scope):
///  * the reference implementation exposes the matcher through a
///    `TwoPhaseIterator` whose block-max and impacts entry points can prune
///    blocks; the port's scorers have no impacts, so this scorer reports
///    [`TwoPhaseState::No`] and does the full match in [`next_doc`] /
///    [`advance`], while still answering [`matches`] correctly;
///  * `get_max_score` answers positive infinity, which only disables competitive
///    skipping.
export class PhraseScorer final : public Scorer {
public:
    /// A scorer for `matcher`; `norms` may be null when the field has no norms,
    /// in which case the norm `1` is used, and `sim_scorer` must not be null.
    PhraseScorer(std::unique_ptr<PhraseMatcher> matcher, std::shared_ptr<const SimScorer> sim_scorer,
                 std::unique_ptr<NumericDocValues> norms);

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    // ---- Scorer ----

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_; }
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override;
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }

    /// Computes the phrase frequency of the current document; the score is that
    /// frequency combined with the norm.
    [[nodiscard]] Result<bool> matches() override;
    [[nodiscard]] float match_cost() const override;
    [[nodiscard]] int32_t approx_doc_id() const override { return matcher_->doc_id(); }
    [[nodiscard]] Result<int32_t> approx_next_doc() override;
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> approx_cost() const override;

    // ---- Scorable ----

    [[nodiscard]] Result<float> score() override;

    /// The matcher of this scorer; exposed for the `matches()` entry points of
    /// the query level machinery.
    [[nodiscard]] const PhraseMatcher &matcher() const noexcept { return *matcher_; }

    /// The phrase frequency of [`doc`], `0` until [`matches`] ran.
    [[nodiscard]] float freq() const noexcept { return freq_; }

private:
    /// Advances the candidate cursor and confirms the match, so that [`doc_id`]
    /// only ever reports documents the phrase really matches.
    [[nodiscard]] Result<int32_t> do_next(int32_t target);

    std::unique_ptr<PhraseMatcher> matcher_;
    std::shared_ptr<const SimScorer> sim_scorer_;
    std::unique_ptr<NumericDocValues> norms_;
    int32_t doc_{-1};
    /// The phrase frequency of [`doc`]; `0` until [`matches`] ran.
    float freq_{0.0F};
    /// The document [`freq`] belongs to, so that the phrase is only computed
    /// once per candidate.
    int32_t freq_doc_{-1};
};

} // namespace lucene
