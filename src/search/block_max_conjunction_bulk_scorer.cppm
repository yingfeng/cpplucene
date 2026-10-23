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

export module lucene_core:block_max_conjunction_bulk_scorer;

import :types;
import std;

import :bits;
import :bulk_scorer;
import :error;
import :leaf_collector;
import :scorable;
import :scorer;

namespace lucene {

/// The [`Scorable`] of a block-max conjunction: the score of the document being
/// collected, plus the minimum competitive score the collector asked for.
///
/// Port of `DocAndScore` (`core/search/block_max_conjunction_bulk_scorer.rs`).
/// **Deviation:** the reference implementation's score *field* is called
/// `current_score` here, because `score()` is the method it overrides.
export class DocAndScore final : public Scorable {
public:
    explicit DocAndScore(float score) : current_score(score) {}

    /// The score of the document being collected.
    float current_score;
    /// The minimum competitive score, the lowest float until the collector asks
    /// for one.
    float min_competitive_score{std::numeric_limits<float>::lowest()};

    [[nodiscard]] Result<float> score() override { return current_score; }

    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override {
        min_competitive_score = min_score;
        return {};
    }
};

/// [`BulkScorer`] for a top-level conjunction of clauses that have no two-phase
/// iterators.
///
/// Port of `BlockMaxConjunctionBulkScorer`
/// (`core/search/block_max_conjunction_bulk_scorer.rs`).
///
/// It differs from
/// [`BlockMaxConjunctionScorer`](lucene_core:block_max_conjunction_scorer) in
/// that scores are computed on the fly, so that the remaining clauses can be
/// skipped as soon as the total score cannot reach the minimum competitive score
/// any more.  That works well because computing a score is cheaper than decoding
/// a whole block of postings.
///
/// **Deviation:** the bit-set accessors of the wrapped scorers are not ported, as
/// everywhere else in the ported search code.
export class BlockMaxConjunctionBulkScorer final : public BulkScorer {
public:
    /// `max_doc` is the number of documents of the leaf.
    [[nodiscard]] static Result<std::unique_ptr<BlockMaxConjunctionBulkScorer>>
    create(int32_t max_doc, std::vector<std::unique_ptr<Scorer>> scorers);

    [[nodiscard]] Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                        int32_t max) override;

    [[nodiscard]] Result<int64_t> cost() override;

    /// The clauses, the cheapest first.
    [[nodiscard]] const std::vector<std::unique_ptr<Scorer>> &scorers() const noexcept { return scorers_; }

    /// The scorable handed to the collector.
    [[nodiscard]] DocAndScore &scorable() noexcept { return scorable_; }

private:
    BlockMaxConjunctionBulkScorer(int32_t max_doc, std::vector<std::unique_ptr<Scorer>> scorers);

    /// Shallow-advances every clause to `window_min` and computes the maximum
    /// score of the window `[window_min, window_max]`, filling the suffix sums of
    /// the clause maxima on the way.
    [[nodiscard]] Result<float> compute_max_score(int32_t window_min, int32_t window_max);

    /// Collects the competitive documents of `[min, max)`.
    [[nodiscard]] VoidResult score_window(LeafCollector &collector, const Bits *accept_docs, int32_t min, int32_t max,
                                          float max_window_score);

    DocAndScore scorable_;
    /// The suffix sums of the clause maximum scores: `sum_of_other_clauses_[i]`
    /// is the sum of the maxima of the clauses `i .. end`.
    std::vector<double> sum_of_other_clauses_;
    int32_t max_doc_;
    std::vector<std::unique_ptr<Scorer>> scorers_;
};

} // namespace lucene
