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

export module lucene_core:conjunction_bulk_scorer;

import :types;
import std;

import :bulk_scorer;
import :error;
import :leaf_collector;
import :scorable;
import :scorer;

namespace lucene {

export class Bits;

/// [`BulkScorer`] for a conjunction of clauses: the cheapest clause leads the
/// iteration and the others are advanced to its documents.
///
/// Port of `ConjunctionBulkScorer` (`core/search/conjunction_bulk_scorer.rs`).
///
/// For simplicity it focuses on scorers that produce regular document iterators
/// rather than two-phase ones: a clause that needs confirmation is confirmed by
/// the collector's competitive iterator, not by this scorer.
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation's `ScorableImpl` borrow is a small helper
///    object here, since the bulk scorer itself cannot be re-borrowed while it is
///    being iterated;
///  * the collector's competitive iterator is fetched once per `score` call
///    instead of once per document.
export class ConjunctionBulkScorer final : public BulkScorer {
public:
    /// `required_scoring` holds the clauses that score, `required_no_scoring` the
    /// ones that only filter; there must be at least two clauses in total.
    [[nodiscard]] static Result<std::unique_ptr<ConjunctionBulkScorer>>
    create(std::vector<std::unique_ptr<Scorer>> required_scoring,
           std::vector<std::unique_ptr<Scorer>> required_no_scoring);

    [[nodiscard]] Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                        int32_t max) override;

    [[nodiscard]] Result<int64_t> cost() override;

    /// The clauses, the cheapest first.
    [[nodiscard]] const std::vector<std::unique_ptr<Scorer>> &scorers() const noexcept { return all_scores_; }

    /// How many of the clauses score; they are the input prefix, which the sorted
    /// array reaches through the index map.
    [[nodiscard]] size_t required_scoring_len() const noexcept { return required_scoring_len_; }

    /// For every clause, its index in the cost-sorted array.
    [[nodiscard]] const std::vector<size_t> &all_scores_input_idx() const noexcept { return all_scores_input_idx_; }

private:
    ConjunctionBulkScorer(std::vector<std::unique_ptr<Scorer>> all_scores, size_t required_scoring_len,
                          std::vector<size_t> all_scores_input_idx);

    [[nodiscard]] Scorer &clause(size_t index) noexcept { return *all_scores_[index]; }

    std::vector<std::unique_ptr<Scorer>> all_scores_;
    size_t required_scoring_len_;
    std::vector<size_t> all_scores_input_idx_;
};

} // namespace lucene
