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

export module lucene_core:constant_score_query;

import :types;
import std;

import :error;
import :query;
import :score_mode;
import :weight;

namespace lucene {

/// Wraps a query so that every matching document gets the same score.
///
/// Port of `ConstantScoreQuery` (`core/search/constant_score_query.rs`).
///
/// **Deviations:** the weight scores the wrapped query with a
/// [`ConstantScoreScorer`] built from its `DocIdSetIterator`, so a wrapped scorer
/// that needs two-phase confirmation is not honoured (see the deviation note of
/// [`ConstantScoreScorer`]); the reference implementation has a two-phase variant.
export class ConstantScoreQuery final : public Query {
public:
    explicit ConstantScoreQuery(std::shared_ptr<Query> query);

    [[nodiscard]] const std::shared_ptr<Query> &query() const noexcept { return query_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    /// Rewrites the wrapped query; when that produces a constant score query too,
    /// the inner one is returned.
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    std::shared_ptr<Query> query_;
};

} // namespace lucene
