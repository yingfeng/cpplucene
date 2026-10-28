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

export module lucene_core:wand_scorer;

import :types;
import std;

import :disi_priority_queue;
import :disi_wrapper;
import :error;
import :scorable;
import :score_mode;
import :scorer;
import :two_phase_iterator;

namespace lucene {

/// Implements the WAND (Weak AND) algorithm for dynamic pruning.
///
/// Port of `WANDScorer` (`core/search/wand_scorer.rs`), which implements
/// “Efficient Query Evaluation using a Two-Level Retrieval Process” by Broder,
/// Carmel, Herscovici, Soffer and Zien, enhanced with the block-max techniques of
/// “Faster Top-k Document Retrieval Using Block-Max Indexes” by Ding and Suel.
///
/// For [`ScoreMode::TopScores`] the scorer keeps a feedback loop with the
/// collector: the minimum competitive score lets it skip documents whose
/// `∑ max_score` cannot reach it.  [`set_min_competitive_score`] drives that, and
/// `min_should_match` is enforced at the same time.
///
/// The sub-scorers live in three places:
///  * **tail**: a max-heap of the scorers that are *behind* the candidate,
///    ordered by the score they may add (ties broken by cost);
///  * **lead**: a linked list of the scorers positioned on the candidate;
///  * **head**: a priority queue of the scorers *beyond* the candidate, ordered by
///    document so the next candidate is cheap to find.
///
/// **Deviations:** `TOP_SCORES` is a runtime flag (the port's `Scorer` is not a
/// template) and the debug-only consistency checks of the reference
/// implementation are left out.
export class WandScorer final : public Scorer {
public:
    /// `sub_scorers` must hold at least two scorers and `min_should_match` must
    /// be in `[1, sub_scorers.size()]`.
    [[nodiscard]] static Result<std::unique_ptr<WandScorer>>
    create(std::vector<std::unique_ptr<Scorer>> sub_scorers, int32_t min_should_match, ScoreMode score_mode,
           int64_t lead_cost);

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }

    // ---- Scorer ----

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_; }
    [[nodiscard]] Result<float> get_max_score(int32_t upto) override;
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::Yes; }
    [[nodiscard]] Result<bool> matches() override;
    [[nodiscard]] float match_cost() const override { return static_cast<float>(sub_scorers_.size()); }
    [[nodiscard]] Result<float> score() override;
    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override;

    // ---- approximation (the candidate cursor is the scorer's own cursor) ----

    [[nodiscard]] int32_t approx_doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> approx_next_doc() override { return next_doc(); }
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override { return advance(target); }
    [[nodiscard]] Result<int64_t> approx_cost() const override { return cost_; }

    /// How many sub-scorers are positioned on the current document.
    [[nodiscard]] int32_t freq() const noexcept { return freq_; }
    /// How many sub-scorers are waiting in the tail.
    [[nodiscard]] size_t tail_size() const noexcept { return tail_size_; }
    /// The scaling factor applied to the maximum scores.
    [[nodiscard]] int32_t scaling_factor() const noexcept { return scaling_factor_; }

private:
    WandScorer(std::vector<std::unique_ptr<Scorer>> sub_scorers, std::vector<DisiWrapper<Scorer *>> wrappers,
               int32_t min_should_match, ScoreMode score_mode, int64_t lead_cost, int32_t scaling_factor);

    /// Adds `lead_idx` to the lead list and accumulates its score.
    [[nodiscard]] VoidResult add_lead(size_t lead_idx);
    /// Adds `lead_idx` to the lead list without scoring it.
    void add_unpositioned_lead(size_t lead_idx);
    /// Moves every lead back to the tail (or the head) at `target`.
    [[nodiscard]] VoidResult push_back_leads(int32_t target);
    /// Advances the head entries that are behind `target`.
    [[nodiscard]] Result<std::optional<size_t>> advance_head(int32_t target);
    /// Advances the tail entry `idx` to the current document.
    [[nodiscard]] VoidResult advance_tail(size_t idx);
    /// Pops the tail entry with the greatest score contribution and advances it.
    [[nodiscard]] VoidResult advance_tail_top();
    /// Recomputes the window boundary and the maximum scores of every window.
    [[nodiscard]] VoidResult update_max_scores(int32_t target);
    /// Moves the window boundary past `target` and the current head.
    [[nodiscard]] VoidResult move_to_next_block(int32_t target);
    /// Sets the document to the next candidate and moves its scorers to the lead.
    [[nodiscard]] VoidResult move_to_next_candidate();
    /// Advances every tail entry to the current document.
    [[nodiscard]] VoidResult advance_all_tail();
    /// Inserts `idx` into the tail, evicting the least useful entry when full.
    [[nodiscard]] std::optional<size_t> insert_tail_with_overflow(size_t idx);
    void add_tail(size_t idx);
    [[nodiscard]] size_t pop_tail();

    [[nodiscard]] bool greater_max_score(size_t left, size_t right) const;
    void up_heap_max_score(size_t i);
    void down_heap_max_score();

    [[nodiscard]] std::optional<size_t> head_top() const { return head_.top(); }
    [[nodiscard]] int32_t head_doc(size_t idx) const { return wrappers_[idx].doc(); }
    [[nodiscard]] Scorer &scorer_at(size_t idx) const { return *wrappers_[idx].scorer(); }

    /// The scorers, owned.
    std::vector<std::unique_ptr<Scorer>> sub_scorers_;
    /// The wrappers, index-parallel to `sub_scorers_`.
    std::vector<DisiWrapper<Scorer *>> wrappers_;

    int32_t doc_{-1};
    ScoreMode score_mode_;
    /// The last document of the window whose maximum scores are known.
    int32_t upto_{-1};
    DisiPriorityQueue<Scorer *> head_;
    std::optional<size_t> lead_;
    int32_t freq_{0};
    int64_t lead_cost_{0};
    double lead_score_{0};
    std::vector<size_t> tail_;
    size_t tail_size_{0};
    int64_t tail_max_score_{0};
    int64_t min_competitive_score_{0};
    int32_t min_should_match_{1};
    int64_t cost_{0};
    int32_t scaling_factor_{0};
};

/// The number of mantissa bits of a single-precision float; maximum scores are
/// scaled so their significant bits fit in it.
inline constexpr int32_t FLOAT_MANTISSA_BITS = 24;

/// The largest value a scaled maximum score may take.
inline constexpr int64_t MAX_SCALED_SCORE = (int64_t{1} << 24) - 1;

/// A scaling factor such that `f * 2^scaling_factor` falls in `[2^23, 2^24)`.
///
/// Special cases: `scaling_factor(0) == scaling_factor(MIN_VALUE) + 1` and
/// `scaling_factor(+inf) == scaling_factor(MAX_VALUE) - 1`, so that the
/// significant bits of any value fit the mantissa.
[[nodiscard]] Result<int32_t> wand_scaling_factor(float value);

/// Scales a maximum score into an integer, rounding **up** so no competitive
/// match is missed.
[[nodiscard]] int64_t wand_scale_max_score(float max_score, int32_t scaling_factor);

/// Scales a minimum competitive score, rounding **down**.
[[nodiscard]] int64_t wand_scale_min_score(float min_score, int32_t scaling_factor);

} // namespace lucene
