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
import :impacts_disi;
import :impacts_enum;
import :max_score_cache;
import :numeric_doc_values;
import :postings_enum;
import :scorable;
import :scorer;
import :similarity;

namespace lucene {

/// Scorer for documents matching one term.
///
/// Port of `TermScorer` (`core/search/term_scorer.rs`).  Two states are
/// collapsed into one class:
///  * over a plain [`PostingsEnum`] (the `PostingsTermState` of the reference
///    implementation), which cannot skip anything;
///  * over an [`ImpactsEnum`], where a [`MaxScoreCache`] turns the indexed
///    impacts into block maximum scores.  When the scorer is the top-level
///    scoring clause, the documents are iterated *through* an [`ImpactsDISI`],
///    so the minimum competitive score of the collector prunes whole blocks.
///
/// **Deviations** (C++ mechanics only):
///  * `SlowImpactsEnum` (an impacts view over a plain postings enum) is not
///    ported: a plain postings enum simply disables pruning;
///  * `ScorerEnum2` / `ScorerEnum4` collapse into this one class;
///  * the bit-set accessors of the wrapped iterator are forwarded verbatim.
export class TermScorer final : public Scorer {
public:
    /// `postings` must be positioned on the leaf's term; `norms` may be null,
    /// and `sim_scorer` scores `(freq, norm)` pairs.
    TermScorer(std::unique_ptr<PostingsEnum> postings, std::shared_ptr<SimScorer> sim_scorer,
               std::unique_ptr<NumericDocValues> norms);

    /// A scorer that can skip non-competitive blocks.  With
    /// `top_level_scoring_clause` the documents are iterated through the
    /// [`ImpactsDISI`], so [`set_min_competitive_score`] takes effect.
    [[nodiscard]] static Result<std::unique_ptr<TermScorer>>
    from_impacts(std::unique_ptr<ImpactsEnum> impacts_enum, std::shared_ptr<SimScorer> sim_scorer,
                 std::unique_ptr<NumericDocValues> norms, bool top_level_scoring_clause);

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    /// Document frequency of the current document; must be positioned first.
    [[nodiscard]] Result<int32_t> freq();

    /// Whether this scorer can skip non-competitive blocks.
    [[nodiscard]] bool has_impacts() const noexcept { return max_score_cache_ != nullptr; }

    // ---- Scorer ----

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_id(); }

    /// The maximum score of the current block, or `+infinity` without impacts.
    [[nodiscard]] Result<float> get_max_score(int32_t upto) override;

    /// Shallow-advances to the block that contains `target`, or answers
    /// `NO_MORE_DOCS` without impacts (nothing can be skipped).
    [[nodiscard]] Result<int32_t> advance_shallow(int32_t target) override;

    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }

    /// The postings themselves, when this scorer only forwards to them.  With an
    /// impacts enum or an impacts iterator in use the cursor can drift from the
    /// postings (the impacts view is what decides which blocks are visited), so
    /// nothing is handed out then.
    [[nodiscard]] DocIdSetIterator *matching_iterator() override {
        if (impacts_disi_ != nullptr || impacts_enum_ != nullptr) {
            return nullptr;
        }
        // The caller drives the postings from now on, so the cached document of
        // [`score`] would be stale.
        handed_out_postings_ = true;
        return postings_.get();
    }

    // ---- Scorable ----

    [[nodiscard]] Result<float> score() override;

    /// Whether [`fast_score`] can be used: BM25 similarity, a direct norms view
    /// and a single postings source, i.e. the shape of every plain term query.
    [[nodiscard]] bool has_fast_score() const noexcept { return fast_score_ok_; }

    /// [`score`] without the state tests: no branches on which similarity or
    /// norms encoding is in use, just one postings call, one norm load and the
    /// BM25 factor.  Only valid when [`has_fast_score`].
    [[nodiscard]] Result<float> fast_score();

    /// The score of `doc_id` as if the term did not occur in it.
    [[nodiscard]] Result<float> smoothing_score(int32_t doc_id) override;

    /// Only the top-level scoring clause can prune; the others keep the default
    /// no-op behaviour.
    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override;

    /// The current term frequency and encoded norm, for `explain`.
    [[nodiscard]] Result<std::pair<int32_t, int64_t>> freq_and_norm();

private:
    /// Resolves the direct value views that let [`score`] skip the virtual calls
    /// of the norms and similarity interfaces.  Called once per scorer.
    void prepare_fast_path();

    /// The document the scorer is positioned on: the cached one, unless the
    /// postings were handed out and the caller keeps them in step.
    [[nodiscard]] int32_t current_doc() const noexcept {
        return handed_out_postings_ ? cursor().doc_id() : doc_;
    }

    /// The iterator whose documents are visited: the impacts iterator when
    /// pruning is possible, the impacts enum when only the impacts are used, the
    /// postings otherwise.
    [[nodiscard]] DocIdSetIterator &cursor() noexcept {
        if (impacts_disi_ != nullptr) {
            return static_cast<DocIdSetIterator &>(*impacts_disi_);
        }
        if (impacts_enum_ != nullptr) {
            return static_cast<DocIdSetIterator &>(*impacts_enum_);
        }
        return static_cast<DocIdSetIterator &>(*postings_);
    }

    [[nodiscard]] const DocIdSetIterator &cursor() const noexcept {
        if (impacts_disi_ != nullptr) {
            return static_cast<const DocIdSetIterator &>(*impacts_disi_);
        }
        if (impacts_enum_ != nullptr) {
            return static_cast<const DocIdSetIterator &>(*impacts_enum_);
        }
        return static_cast<const DocIdSetIterator &>(*postings_);
    }

    std::unique_ptr<PostingsEnum> postings_;
    std::shared_ptr<SimScorer> sim_scorer_;
    std::unique_ptr<NumericDocValues> norms_;
    /// Set when the scorer iterates an impacts enum instead of `postings_`.
    std::unique_ptr<ImpactsEnum> impacts_enum_;
    std::unique_ptr<MaxScoreCache> max_score_cache_;
    /// Set only for the top-level scoring clause.
    std::unique_ptr<ImpactsDISI> impacts_disi_;

    /// The document the scorer is positioned on, kept here instead of asking the
    /// cursor, like the `doc` field of the reference implementation.
    int32_t doc_{-1};
    /// Set once [`matching_iterator`] hands the postings out: the caller then
    /// advances them itself, so `doc_` no longer follows.
    bool handed_out_postings_{false};

    // ---- the direct scoring path (see `prepare_fast_path`) ----

    /// The norms as raw bytes; `value_at` answers `false` for a document without
    /// one, which the scoring path reads as the neutral norm.  Set when the
    /// encoding is dense or constant, *and* when the field has no norms at all.
    DirectNumericValues norms_view_{};
    /// Whether [`norms_view`] is usable; the sparse encoding has no direct view
    /// and asks [`NumericDocValues::exact_value`] instead.
    bool norms_direct_{false};
    /// `weight` and `1 / (k1 * (1 - b + b * dl / avgdl))` of a BM25 scorer, or
    /// null when the similarity is not BM25.
    float bm25_weight_{0.0F};
    const float *bm25_norm_inverse_{nullptr};
    /// The postings enum [`freq`] reads from: the impacts enum when there is one,
    /// the postings otherwise.  Resolved once so [`fast_score`] has no choice to
    /// make.
    PostingsEnum *freq_source_{nullptr};
    /// [`has_fast_score`], resolved once at construction.
    bool fast_score_ok_{false};
    /// [`freq_source`] as the concrete block postings type, when it is one.
    ///
    /// Typed as `void *` so this interface does not have to depend on the codec
    /// modules; `prepare_fast_path` stores the pointer and the implementation
    /// knows what it is.  Calling `freq()` on the concrete type is a static call,
    /// which is what makes [`fast_score`] smaller than a virtual dispatch.
    void *freq_source_concrete_{nullptr};
};

} // namespace lucene
