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

export module lucene_core:max_score_bulk_scorer;

import :types;
import std;

import :bits;
import :bulk_scorer;
import :disi_priority_queue;
import :disi_wrapper;
import :error;
import :leaf_collector;
import :scorable;
import :scorer;

namespace lucene {

/// The width of the inner windows, in documents.
inline constexpr int32_t kMaxScoreInnerWindowSize = 1 << 12;

/// The [`Scorable`] of a max-score disjunction: the score of the document being
/// collected plus the minimum competitive score the collector asked for.
///
/// Port of `Score` (`core/search/max_score_bulk_scorer.rs`).
export class MaxScoreScorable final : public Scorable {
public:
    [[nodiscard]] Result<float> score() override { return current_score; }

    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override {
        min_competitive_score = min_score;
        return {};
    }

    /// The score of the document being collected.
    float current_score{0.0F};
    /// The minimum competitive score, `0` until the collector asks for one.
    float min_competitive_score{0.0F};
};

/// [`BulkScorer`] for a disjunction that is evaluated with the MAXSCORE
/// algorithm: clauses are split into non-essential ones (whose maximum scores
/// can't break the minimum competitive score on their own) and essential ones.
///
/// Port of `MaxScoreBulkScorer` (`core/search/max_score_bulk_scorer.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation keeps six inner-window strategies
///    (`as_conjunction`, `single_essential_clause`, `with_filter`, ...) to make
///    the common shapes faster; the port keeps the general
///    "multiple essential clauses" strategy for every shape, which visits the
///    same documents with the same scores, and applies the optional filter while
///    the window is emitted;
///  * the generic `S1`/`S2` (clause, filter) types collapse into
///    `std::unique_ptr<Scorer>`.
export class MaxScoreBulkScorer final : public BulkScorer {
public:
    /// `scorers` must not be empty; `filter` may be null.
    [[nodiscard]] static Result<std::unique_ptr<MaxScoreBulkScorer>>
    create(int32_t max_doc, std::vector<std::unique_ptr<Scorer>> scorers, std::unique_ptr<Scorer> filter);

    [[nodiscard]] Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                        int32_t max) override;

    [[nodiscard]] Result<int64_t> cost() override;

    /// The scorable handed to the collector.
    [[nodiscard]] MaxScoreScorable &scorable() noexcept { return scorable_; }

    /// How many scorers are non-essential, i.e. the index of the first essential
    /// one.
    [[nodiscard]] size_t first_essential_scorer() const noexcept { return first_essential_scorer_; }

    /// The index of the first scorer that a competitive hit must match.
    [[nodiscard]] size_t first_required_scorer() const noexcept { return first_required_scorer_; }

    /// The clauses, in input order.
    [[nodiscard]] const std::vector<DisiWrapper<std::unique_ptr<Scorer>>> &all_scorers() const noexcept {
        return all_scorers_;
    }

private:
    MaxScoreBulkScorer(int32_t max_doc, std::vector<std::unique_ptr<Scorer>> scorers,
                       std::unique_ptr<Scorer> filter);

    /// Refreshes the maximum score of every clause for the outer window.
    [[nodiscard]] VoidResult update_max_window_scores(int32_t window_min, int32_t window_max);

    /// Splits the clauses into non-essential and essential ones for the current
    /// minimum competitive score; answers whether any clause is essential.
    [[nodiscard]] Result<bool> partition_scorers();

    /// The end (exclusive) of the outer window that starts at `window_min`.
    [[nodiscard]] Result<int32_t> compute_outer_window_max(int32_t window_min);

    /// Scores a candidate that the essential clauses matched with
    /// `essential_score`.
    [[nodiscard]] VoidResult score_non_essential_clauses(LeafCollector &collector, int32_t doc, double essential_score,
                                                         size_t num_non_essential_clauses);

    /// Collects the candidates of one inner window that starts at the current
    /// essential top.
    [[nodiscard]] VoidResult score_inner_window(LeafCollector &collector, const Bits *accept_docs, int32_t max);

    /// The next candidate on or after `range_end`.
    [[nodiscard]] int32_t next_candidate(int32_t range_end) const;

    int32_t max_doc_;
    std::vector<DisiWrapper<std::unique_ptr<Scorer>>> all_scorers_;
    /// The clause indices, sorted by increasing maximum score.
    std::vector<size_t> all_scorers_idx_;
    std::vector<size_t> scratch_;
    /// The essential clauses, ordered by document.
    DisiPriorityQueue<std::unique_ptr<Scorer>> essential_queue_;
    size_t first_essential_scorer_{0};
    size_t first_required_scorer_{0};
    /// The minimum competitive score that would produce a better partitioning.
    float next_min_competitive_score_{0.0F};
    int64_t cost_{0};
    MaxScoreScorable scorable_;
    /// The suffix sums of the non-essential maximum window scores.
    std::vector<double> max_score_sums_;
    std::unique_ptr<DisiWrapper<std::unique_ptr<Scorer>>> filter_;
    /// The documents of the inner window, one bit each.
    std::vector<uint64_t> window_matches_;
    /// The essential score of every document of the inner window.
    std::vector<double> window_scores_;
    int32_t num_outer_windows_{0};
    int32_t num_candidates_{0};
    /// The smallest outer window the scorer accepts, in documents.
    ///
    /// **Deviation:** it starts at `1` instead of `-1`, because a left shift of
    /// a negative value is not portable in C++.
    int32_t min_window_size_{1};
};

} // namespace lucene
