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

export module lucene_core:index_or_doc_values_query;

import :types;
import std;

import :error;
import :explanation;
import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :weight;

namespace lucene {

export class IndexOrDocValuesQuery;

/// Matches the same documents as two queries, one over the index and one over doc
/// values, and picks the cheaper one for every segment.
///
/// Port of `IndexOrDocValuesQuery` (`core/search/index_or_doc_values_query.rs`).
///
/// This is how a range query can use the index (points) where that is cheap and
/// doc values where the index would have to visit many documents.  Both wrapped
/// queries must match the same documents and produce the same scores.
export class IndexOrDocValuesQuery final : public Query {
public:
    /// The pair of queries: `index_query` has a good iterator but may be
    /// expensive to set up, `dv_query` is cheap to set up but may be slow to
    /// enumerate.
    [[nodiscard]] static Result<IndexOrDocValuesQuery> create(std::shared_ptr<Query> index_query,
                                                             std::shared_ptr<Query> dv_query);

    /// The query over the index.
    [[nodiscard]] const std::shared_ptr<Query> &index_query() const noexcept { return index_query_; }
    /// The query over doc values.
    [[nodiscard]] const std::shared_ptr<Query> &dv_query() const noexcept { return dv_query_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    /// The visitor sees the query over the index, like the reference
    /// implementation.
    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `IndexOrDocValuesQuery(indexQuery=..., dvQuery=...)`.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    IndexOrDocValuesQuery(std::shared_ptr<Query> index_query, std::shared_ptr<Query> dv_query);

    std::shared_ptr<Query> index_query_;
    std::shared_ptr<Query> dv_query_;
};

/// The weight of an [`IndexOrDocValuesQuery`]: it builds both weights and asks
/// both for a supplier per leaf, then keeps the cheaper one.
///
/// Port of `IndexOrDocValuesQueryWeight`
/// (`core/search/index_or_doc_values_query.rs`).
export class IndexOrDocValuesWeight final : public Weight {
public:
    /// Builds both wrapped weights.
    [[nodiscard]] static Result<std::unique_ptr<IndexOrDocValuesWeight>>
    create(const IndexOrDocValuesQuery &query, const IndexSearcher &searcher, ScoreMode score_mode, float boost);

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override;

    [[nodiscard]] const Query &get_query() const override { return query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher &searcher) override;

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    IndexOrDocValuesWeight(const IndexOrDocValuesQuery &query, std::unique_ptr<Weight> index_weight,
                           std::unique_ptr<Weight> dv_weight);

    const IndexOrDocValuesQuery &query_;
    std::unique_ptr<Weight> index_weight_;
    std::unique_ptr<Weight> dv_weight_;
};

} // namespace lucene
