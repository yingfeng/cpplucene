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

export module lucene_core:boolean_scorer_supplier;

import :types;

import std;

import :boolean_clause;
import :error;
import :leaf_reader_context;
import :score_mode;
import :scorer;
import :scorer_supplier;

namespace lucene {

export class IndexSearcher;

/// Assembles the scorer of a [`BooleanQuery`](lucene_core:boolean_query) out of
/// the suppliers of its clauses.
///
/// Port of `BooleanScorerSupplier` (`core/search/boolean_scorer_supplier.rs`).
///
/// The combinations, in the reference implementation's order:
///  * only `SHOULD`/`MUST_NOT`: a disjunction, optionally wrapped for the
///    prohibited clauses;
///  * `minimum_should_match > 0` with required clauses: a conjunction of the
///    required part and the "at least N should match" part, both scoring;
///  * otherwise: the required part plus the optional part as
///    `ReqOptSumScorer` (the optional clauses only add score).
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation selects between a plain scorer and a `BooleanScorer` (window based,
///    for dense optional clauses) and between block-max / WAND / MAX_SCORE
///    variants; the port always builds the plain scorers, which is exact but
///    slower (all of those need impacts or a bitset window);
///  * `disable_scoring`, `RequiredOptionalMinimumShouldMatchScorer` and the
///    constant-score wrapping of filters are folded into
///    [`ConstantScoreScorer`](lucene_core:constant_score_scorer) /
///    [`MinimumShouldMatchScorer`](lucene_core:minimum_should_match_scorer).
export class BooleanScorerSupplier final : public ScorerSupplier {
public:
    /// `subs` is indexed by `static_cast<size_t>(Occur)`:
    /// `0 = MUST, 1 = FILTER, 2 = SHOULD, 3 = MUST_NOT`.
    [[nodiscard]] static Result<std::unique_ptr<BooleanScorerSupplier>>
    create(std::array<std::vector<std::unique_ptr<ScorerSupplier>>, 4> subs, ScoreMode score_mode,
           int32_t min_should_match, int32_t max_doc);

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t lead_cost, const LeafReaderContext &context,
                                                      const IndexSearcher &searcher) override;

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext &context, const IndexSearcher &searcher) override;

    [[nodiscard]] VoidResult set_top_level_scoring_clause() override;

    [[nodiscard]] int32_t min_should_match() const noexcept { return min_should_match_; }

private:
    BooleanScorerSupplier(std::array<std::vector<std::unique_ptr<ScorerSupplier>>, 4> subs, ScoreMode score_mode,
                          int32_t min_should_match, int32_t max_doc)
        : subs_(std::move(subs)), score_mode_(score_mode), min_should_match_(min_should_match), max_doc_(max_doc) {}

    /// The required part (an intersection of the `FILTER` and `MUST` clauses).
    [[nodiscard]] Result<std::unique_ptr<Scorer>> required(int64_t lead_cost, const LeafReaderContext &context,
                                                           const IndexSearcher &searcher);

    /// The disjunction of the clauses with `occur`, honouring `min_should_match`.
    [[nodiscard]] Result<std::unique_ptr<Scorer>> optional(Occur occur, int32_t min_should_match, int64_t lead_cost,
                                                           const LeafReaderContext &context,
                                                           const IndexSearcher &searcher);

    /// Wraps `scorer` so that its documents are rejected by the `MUST_NOT`
    /// clauses.
    [[nodiscard]] Result<std::unique_ptr<Scorer>> exclude(std::unique_ptr<Scorer> scorer, int64_t lead_cost,
                                                          const LeafReaderContext &context,
                                                          const IndexSearcher &searcher);

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get_internal(int64_t lead_cost, const LeafReaderContext &context,
                                                               const IndexSearcher &searcher);

    std::array<std::vector<std::unique_ptr<ScorerSupplier>>, 4> subs_;
    ScoreMode score_mode_;
    int32_t min_should_match_;
    int32_t max_doc_;
    bool top_level_scoring_clause_{false};
};

} // namespace lucene
