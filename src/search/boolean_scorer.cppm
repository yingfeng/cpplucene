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

export module lucene_core:boolean_scorer;

import :bits;
import :bulk_scorer;
import :doc_id_set_iterator;
import :doc_id_stream;
import :error;
import :leaf_collector;
import :priority_queue;
import :scorable;
import :scorer;
import :scorer_util;
import :types;

import std;

namespace lucene {

/// Documents per scoring window.
export inline constexpr size_t BOOLEAN_SCORER_SHIFT = 12;
/// `1 << SHIFT`, i.e. how many documents one window holds.
export inline constexpr size_t BOOLEAN_SCORER_SIZE = size_t{1} << BOOLEAN_SCORER_SHIFT;
/// `SIZE - 1`, the document offset inside a window.
export inline constexpr size_t BOOLEAN_SCORER_MASK = BOOLEAN_SCORER_SIZE - 1;
/// Number of `uint64_t` words the window's bit set needs.
export inline constexpr size_t BOOLEAN_SCORER_SET_SIZE = size_t{1} << (BOOLEAN_SCORER_SHIFT - 6);

/// [`BulkScorer`] for pure disjunctions (and disjunctions with a low
/// `minimum_should_match`) whose clauses are dense.
///
/// Port of `search/boolean_scorer.rs`.  Documents are scored window by window
/// (4096 at a time): every clause writes the documents it matches inside the
/// window into a bit set, the per-document frequencies and scores accumulate in
/// [`Bucket`]s, and the window is then replayed through a [`DocIdStream`] (or
/// collected straight from the single clause, when only one can match).
///
/// **Deviations:**
///  * the reference implementation wraps each clause in `DisiWrapper` and keeps
///    it in two `PriorityQueue`s; this port keeps a plain `Clause` (scorer, doc,
///    cost) because `DisiWrapper`'s linked-list fields are only needed by the
///    max-score scorers;
///  * the scorer is not yet chosen by `BooleanScorerSupplier`; see that class's
///    note.  The oracle test (`boolean_scorer_window_ut.cpp`) compares it against
///    the plain combinators before it is wired in.
export class BooleanScorer final : public BulkScorer {
public:
    /// Per-document accumulator inside one window.
    struct Bucket {
        float score{0.0F};
        size_t freq{0};
    };

    /// The [`DocIdStream`] a window is replayed through.
    ///
    /// Port of `DocIdStreamView`.
    class Stream final : public DocIdStream {
    public:
        Stream(BooleanScorer &scorer, int32_t base) : scorer_(&scorer), base_(base) {}

        [[nodiscard]] VoidResult for_each(DocIdStreamConsumer &consumer) override;

        [[nodiscard]] Result<int32_t> count(Scorable &scorer) override;

    private:
        BooleanScorer *scorer_;
        int32_t base_;
    };

    /// Builds a scorer over `scorers`; at least two clauses are required.
    [[nodiscard]] static Result<std::unique_ptr<BooleanScorer>> create(std::vector<std::unique_ptr<Scorer>> scorers,
                                                                      size_t min_should_match, bool needs_scores);

    [[nodiscard]] Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                        int32_t max) override;

    [[nodiscard]] Result<int64_t> cost() override { return cost_; }

    [[nodiscard]] size_t min_should_match() const noexcept { return min_should_match_; }
    [[nodiscard]] bool needs_scores() const noexcept { return needs_scores_; }
    [[nodiscard]] size_t num_scorers() const noexcept { return head_.size() + tail_.size() + leads_.size(); }

    /// The score of the document last replayed from a window.
    [[nodiscard]] Result<float> current_score() { return score_.score(); }
    [[nodiscard]] VoidResult set_current_score(float score) { return score_.set_score(score); }

    /// Moves the head to the first document `>= min` and returns its position.
    [[nodiscard]] Result<int32_t> advance(int32_t min);

private:
    BooleanScorer(size_t min_should_match, bool needs_scores, bool with_buckets, int64_t cost);

    /// One clause of the disjunction.
    struct Clause {
        std::unique_ptr<Scorer> scorer;
        int32_t doc{-1};
        int64_t cost{0};
    };

    /// Orders the head queue by document.
    struct HeadCompare {
        [[nodiscard]] Result<bool> less_than(const Clause &left, const Clause &right) const {
            return left.doc < right.doc;
        }
    };

    /// Orders the tail queue by cost, so the cheapest clauses stay in the head.
    struct TailCompare {
        [[nodiscard]] Result<bool> less_than(const Clause &left, const Clause &right) const {
            return left.cost < right.cost;
        }
    };

    /// Writes the documents of `clause` that fall into `[min, max)` into the
    /// window bit set (and into the buckets).
    [[nodiscard]] VoidResult score_clause_into_bitset(Clause &clause, const Bits *accept_docs, int32_t min,
                                                      int32_t max);

    /// The window that starts at `top_doc`: collects it and returns the new head.
    [[nodiscard]] Result<Clause *> score_window(int32_t top_doc, LeafCollector &collector, const Bits *accept_docs,
                                                int32_t min, int32_t max);

    /// One clause can match in this window: no bit set, collect directly.
    [[nodiscard]] VoidResult score_window_single_scorer(Clause &clause, LeafCollector &collector,
                                                        const Bits *accept_docs, int32_t window_min,
                                                        int32_t window_max, int32_t max);

    /// Several clauses: fill the bit set, then replay the window through a stream.
    [[nodiscard]] VoidResult score_window_multiple_scorers(LeafCollector &collector, const Bits *accept_docs,
                                                           int32_t window_base, int32_t window_min,
                                                           int32_t window_max, size_t max_freq,
                                                           std::vector<Clause> &leads);

    /// Moves every clause of `leads` back into the queues.
    [[nodiscard]] VoidResult restore_leads(std::vector<Clause> &leads);

    /// Clears the window's bit set and buckets.
    void reset_window() noexcept;

    /// A [`Scorable`] over the score a window replay accumulated.
    class WindowScore final : public Scorable {
    public:
        [[nodiscard]] Result<float> score() override { return score_; }
        [[nodiscard]] VoidResult set_score(float score) override {
            score_ = score;
            return {};
        }

    private:
        float score_{0.0F};
    };

    std::vector<Bucket> buckets_;
    std::array<uint64_t, BOOLEAN_SCORER_SET_SIZE> matching_{};
    PriorityQueue<Clause, HeadCompare> head_;
    PriorityQueue<Clause, TailCompare> tail_;
    std::vector<Clause> leads_;
    WindowScore score_;
    size_t min_should_match_{1};
    int64_t cost_{0};
    bool needs_scores_{false};
};

} // namespace lucene
