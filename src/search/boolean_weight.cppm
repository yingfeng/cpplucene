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

export module lucene_core:boolean_weight;

import :types;

import std;

import :boolean_clause;
import :error;
import :leaf_reader_context;
import :query;
import :score_mode;
import :scorer_supplier;
import :weight;

namespace lucene {

export class BooleanQuery;
export class IndexSearcher;

/// The weight of a [`BooleanQuery`](lucene_core:boolean_query): one sub-weight
/// per clause, plus the assembly of their scorers.
///
/// Port of `BooleanWeight` (`core/search/boolean_weight.rs`).
///
/// **Deviation:** the reference implementation stores the clause index next to each sub-weight
/// (`WeightedBooleanClause`); the port keeps the sub-weights in clause order, so
/// the index is the vector position.
export class BooleanWeight final : public Weight {
public:
    /// Builds the sub-weights: scoring clauses inherit `score_mode`, the others
    /// are built with `CompleteNoScores` because their score is never used.
    [[nodiscard]] static Result<std::unique_ptr<BooleanWeight>>
    create(const BooleanQuery &query, const IndexSearcher &searcher, ScoreMode score_mode, float boost);

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override;

    [[nodiscard]] const Query &get_query() const override;

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher &searcher) override;

    [[nodiscard]] Result<int32_t> count(const LeafReaderContext &context, const IndexSearcher &searcher) override;

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override;

    /// The sub-weights, in clause order.
    [[nodiscard]] const std::vector<std::unique_ptr<Weight>> &sub_weights() const noexcept { return sub_weights_; }

private:
    BooleanWeight(const BooleanQuery &query, ScoreMode score_mode, std::vector<std::unique_ptr<Weight>> sub_weights)
        : query_(&query), score_mode_(score_mode), sub_weights_(std::move(sub_weights)) {}

    /// The number of documents matching the required clauses, `-1` if unknown, or
    /// `num_docs` when there is no required clause.
    [[nodiscard]] Result<int32_t> req_count(const LeafReaderContext &context, const IndexSearcher &searcher);

    /// The number of documents matching the clauses with `occur`, `-1` if
    /// unknown, or `0` when there is no such clause.
    [[nodiscard]] Result<int32_t> opt_count(const LeafReaderContext &context, Occur occur,
                                            const IndexSearcher &searcher);

    const BooleanQuery *query_;
    ScoreMode score_mode_;
    std::vector<std::unique_ptr<Weight>> sub_weights_;
};

} // namespace lucene
