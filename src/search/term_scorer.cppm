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

import :bm25_similarity;
import :doc_id_set_iterator;
import :error;
import :impacts_disi;
import :impacts_enum;
import :lucene101_postings_reader;
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
    ///
    /// Defined here rather than out of line so that the bulk scorer, which holds
    /// the concrete scorer type, inlines it: the concrete block postings read is
    /// then one array access in the scoring loop instead of a call.
    [[gnu::always_inline]] [[nodiscard]] Result<int32_t> freq() {
        if (postings_concrete_ != nullptr) {
            return postings_concrete_->freq();
        }
        if (freq_source_ != nullptr) {
            return freq_source_->freq();
        }
        return postings_->freq();
    }

    /// Whether this scorer can skip non-competitive blocks.
    [[nodiscard]] bool has_impacts() const noexcept { return max_score_cache_ != nullptr; }

    /// [`next_doc`] without the layered virtual calls: the impacts iterator walks
    /// the concrete postings, so the whole chain inlines into the bulk scoring
    /// loop instead of costing three indirect calls per document.
    [[gnu::always_inline]] [[nodiscard]] Result<int32_t> next_doc_inline() {
        if (impacts_disi_ != nullptr) {
            if (postings_concrete_ != nullptr) {
                return remember_doc(impacts_disi_->next_doc_with(*postings_concrete_));
            }
            return remember_doc(impacts_disi_->next_doc());
        }
        if (postings_concrete_ != nullptr) {
            return remember_doc(postings_concrete_->next_doc());
        }
        return next_doc();
    }

    /// [`advance`], inlined the same way as [`next_doc_inline`].
    [[gnu::always_inline]] [[nodiscard]] Result<int32_t> advance_inline(int32_t target) {
        if (impacts_disi_ != nullptr) {
            if (postings_concrete_ != nullptr) {
                return remember_doc(impacts_disi_->advance_with(*postings_concrete_, target));
            }
            return remember_doc(impacts_disi_->advance(target));
        }
        if (postings_concrete_ != nullptr) {
            return remember_doc(postings_concrete_->advance(target));
        }
        return advance(target);
    }

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
    ///
    /// In the interface for the same reason as [`freq`]: the whole body then
    /// inlines into the scoring loop.
    [[gnu::always_inline]] [[nodiscard]] Result<float> fast_score() {
        auto frequency = freq();
        if (!frequency.has_value()) {
            return std::unexpected(std::move(frequency.error()));
        }
        int64_t norm = 1;
        if (!norm_of(current_doc(), norm)) {
            // The document has no norm; the neutral value scores as an empty
            // field.
            norm = 1;
        }
        return bm25_tf_score(bm25_weight_, bm25_norm_inverse_[static_cast<uint8_t>(norm)],
                             static_cast<float>(frequency.value()));
    }

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
        return handed_out_postings_ ? cursor_->doc_id() : doc_;
    }

    /// The norm of `doc`, `false` when it has none.
    ///
    /// The one-byte encoding — what a norms producer writes for every field
    /// whose values fit in a byte — is read with a plain load instead of the
    /// width dispatch of [`DirectNumericValues::value_at`], which the scoring
    /// loop pays once per document.
    [[gnu::always_inline]] [[nodiscard]] bool norm_of(int32_t doc, int64_t &out) const noexcept {
        if (norms_view_.width == 1) {
            if (doc < 0 || doc >= norms_view_.max_doc) {
                return false;
            }
            out = static_cast<int8_t>(norms_view_.bytes[static_cast<size_t>(doc)]);
            return true;
        }
        return norms_view_.value_at(doc, out);
    }

    /// Keeps [`doc`] in step with the inlined walks below, like [`next_doc`]
    /// and [`advance`] do.
    [[gnu::always_inline]] [[nodiscard]] Result<int32_t> remember_doc(Result<int32_t> next) {
        if (next.has_value()) {
            doc_ = next.value();
        }
        return next;
    }

    /// The iterator whose documents are visited: the impacts iterator when
    /// pruning is possible, the impacts enum when only the impacts are used, the
    /// postings otherwise.  Resolved once, in the constructors.
    [[nodiscard]] DocIdSetIterator &cursor() noexcept { return *cursor_; }
    [[nodiscard]] const DocIdSetIterator &cursor() const noexcept { return *cursor_; }

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
    /// The iterator the scorer is driven by; see [`cursor`].
    DocIdSetIterator *cursor_{nullptr};
    /// [`has_fast_score`], resolved once at construction.
    bool fast_score_ok_{false};
    /// The postings as the concrete block postings type, when they are one: the
    /// type the cursor walks and [`freq`] reads.
    ///
    /// Calling it directly is a static call that inlines into the scoring loop,
    /// which is what the reference implementation gets from monomorphising.
    BlockPostingsEnum *postings_concrete_{nullptr};
};

} // namespace lucene
