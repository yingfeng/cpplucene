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

export module lucene_core:term_scorer;

import :types;
import std;

import :doc_id_set_iterator;
import :error;
import :numeric_doc_values;
import :postings_enum;
import :scorable;
import :scorer;
import :similarity;

namespace lucene {

/// Scorer for documents matching one term.
///
/// Port of `TermScorer` (`core/search/term_scorer.rs`), reduced to the postings
/// path.
///
/// **Deviations** (C++ mechanics only, all of them "not ported yet"):
///  * the impacts/`MaxScoreCache` state (`ImpactsTermState`,
///    `SourceImpactsDISI`, `SlowImpactsEnum`) is not ported, so this scorer
///    always iterates the postings list and [`get_max_score`] reports
///    `+infinity` — an upper bound, which only disables competitive skipping and
///    therefore keeps results correct;
///  * `advance_shallow` keeps the default (no block skipping);
///  * the bit-set accessors of the wrapped iterator are forwarded verbatim.
export class TermScorer final : public Scorer {
public:
    /// `postings` must be positioned on the leaf's term; `norms` may be null,
    /// and `sim_scorer` scores `(freq, norm)` pairs.
    TermScorer(std::unique_ptr<PostingsEnum> postings, std::shared_ptr<SimScorer> sim_scorer,
               std::unique_ptr<NumericDocValues> norms);

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    /// Document frequency of the current document; must be positioned first.
    [[nodiscard]] Result<int32_t> freq();

    // ---- Scorer ----

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_id(); }

    /// `+infinity`: a valid upper bound that disables pruning (see the class
    /// note).
    [[nodiscard]] Result<float> get_max_score(int32_t upto) override;

    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }

    // ---- Scorable ----

    [[nodiscard]] Result<float> score() override;

    /// The score of `doc_id` as if the term did not occur in it.
    [[nodiscard]] Result<float> smoothing_score(int32_t doc_id) override;

    /// The current term frequency and encoded norm, for `explain`.
    [[nodiscard]] Result<std::pair<int32_t, int64_t>> freq_and_norm();

private:
    std::unique_ptr<PostingsEnum> postings_;
    std::shared_ptr<SimScorer> sim_scorer_;
    std::unique_ptr<NumericDocValues> norms_;
};

} // namespace lucene
