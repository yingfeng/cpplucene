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

import :doc_id_set_iterator;
import :error;
import :exact_phrase_matcher;
import :impacts_disi;
import :max_score_cache;
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
/// In `ScoreMode::TopScores` the candidate cursor is an [`ImpactsDISI`] over the
/// impacts of the phrase, so blocks whose maximum score cannot compete with the
/// collector's threshold are skipped without decoding any position; the
/// documents that survive are still confirmed by the matcher.  Other modes walk
/// the matcher's conjunction directly.
///
/// **Deviations** (C++ scope):
///  * the reference implementation exposes the matcher through a
///    `TwoPhaseIterator`; the port's [`Scorer`] carries the two-phase entry
///    points itself and this scorer reports [`TwoPhaseState::No`], doing the
///    full match in [`next_doc`] / [`advance`] while still answering
///    [`matches`] correctly;
///  * `get_max_score` answers positive infinity when the phrase has no impacts
///    (a field without frequencies or norms, or a mode that does not need
///    scores), which only disables competitive skipping.
export class PhraseScorer final : public Scorer {
public:
    /// A scorer for `matcher`; `norms` may be null when the field has no norms,
    /// in which case the norm `1` is used, and `sim_scorer` must not be null.
    /// `score_mode` decides whether the impacts of the matcher are used to skip
    /// non-competitive blocks.
    PhraseScorer(std::unique_ptr<PhraseMatcher> matcher, std::shared_ptr<const SimScorer> sim_scorer,
                 std::unique_ptr<NumericDocValues> norms, ScoreMode score_mode);

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    // ---- Scorer ----

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_; }
    [[nodiscard]] Result<int32_t> advance_shallow(int32_t target) override;
    [[nodiscard]] Result<float> get_max_score(int32_t upto) override;
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }

    /// Whether the current document matches; only the *first* match is looked
    /// for, so a query that never scores a document (a count, for instance) does
    /// not enumerate the remaining matches of the phrase.
    ///
    /// The phrase frequency is accumulated by [`score`], the only caller that
    /// needs it.
    [[nodiscard]] Result<bool> matches() override;
    [[nodiscard]] float match_cost() const override;
    [[nodiscard]] int32_t approx_doc_id() const override { return matcher_->doc_id(); }
    [[nodiscard]] Result<int32_t> approx_next_doc() override;
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> approx_cost() const override;

    // ---- Scorable ----

    [[nodiscard]] Result<float> score() override;

    /// Tells the scorer that documents scoring below `min_score` cannot make it
    /// into the top hits, so whole blocks may be skipped.
    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override;

    /// The matcher of this scorer; exposed for the `matches()` entry points of
    /// the query level machinery.
    [[nodiscard]] const PhraseMatcher &matcher() const noexcept { return *matcher_; }

    /// The phrase frequency of [`doc`], `0` until [`matches`] ran and complete
    /// (all matches enumerated) once [`score`] ran.
    [[nodiscard]] float freq() const noexcept { return freq_; }

private:
    /// The candidate cursor, with competitive skipping when the impacts allow
    /// it: either the matcher itself or the impacts iterator that drives it.
    [[nodiscard]] Result<int32_t> next_candidate();
    [[nodiscard]] Result<int32_t> advance_candidate(int32_t target);

    /// Advances the candidate cursor and confirms the match, so that [`doc_id`]
    /// only ever reports documents the phrase really matches.
    [[nodiscard]] Result<int32_t> do_next(int32_t target, bool advance);

    /// The norm of [`doc`], `1` when the field has no norms.
    [[nodiscard]] Result<int64_t> norm_of_current_doc();

    // ---- the matcher, through its concrete type when it has one ----
    //
    // A phrase query enters these four entry points once per candidate
    // document; through the abstract matcher that is four indirect calls per
    // document, and the bodies stay opaque to the caller.  With the concrete
    // matcher they inline into the per-document match, which is what the
    // reference implementation gets from monomorphising.

    [[gnu::always_inline]] [[nodiscard]] VoidResult reset_matcher() {
        if (exact_ != nullptr) {
            return exact_->reset();
        }
        return matcher_->reset();
    }

    [[gnu::always_inline]] [[nodiscard]] Result<bool> next_match_of_matcher() {
        if (exact_ != nullptr) {
            return exact_->next_match();
        }
        return matcher_->next_match();
    }

    [[gnu::always_inline]] [[nodiscard]] Result<float> max_freq_of_matcher() {
        if (exact_ != nullptr) {
            return exact_->max_freq();
        }
        return matcher_->max_freq();
    }

    [[gnu::always_inline]] [[nodiscard]] float match_weight() const {
        if (exact_ != nullptr) {
            return exact_->sloppy_weight();
        }
        return matcher_->sloppy_weight();
    }

    std::unique_ptr<PhraseMatcher> matcher_;
    /// [`matcher`] as the exact matcher, the type of every phrase without slop;
    /// null for a sloppy or multi-term matcher, which take the abstract path.
    ExactPhraseMatcher *exact_{nullptr};
    std::shared_ptr<const SimScorer> sim_scorer_;
    std::unique_ptr<NumericDocValues> norms_;
    ScoreMode score_mode_;

    // The skipping machinery, in construction order: the iterator borrows the
    // two members before it, so they must outlive it.
    std::unique_ptr<DocIdSetIterator> approximation_;
    std::unique_ptr<MaxScoreCache> max_score_cache_;
    std::unique_ptr<ImpactsDISI> impacts_approximation_;

    float min_competitive_score_{0.0F};
    int32_t doc_{-1};
    /// The phrase frequency of [`doc`] as far as it has been accumulated: `0`
    /// when the document does not match, the weight of the first match after
    /// [`matches`] confirmed it, and the complete frequency after [`score`].
    float freq_{0.0F};
    /// Whether [`freq`] is the complete frequency of [`freq_doc`], i.e. whether
    /// every match of the document has been enumerated.
    bool freq_complete_{false};
    /// The document [`freq`] belongs to, so that the phrase is only computed
    /// once per candidate.
    int32_t freq_doc_{-1};
};

} // namespace lucene
