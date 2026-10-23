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

export module lucene_core:boost_query;

import :types;
import std;

import :error;
import :index_searcher;
import :query;
import :query_visitor;
import :score_mode;
import :weight;

namespace lucene {

/// A query wrapper that multiplies the scores of the wrapped query by a boost.
///
/// Port of `BoostQuery` (`core/search/boost_query.rs`).
///
/// A boost below one gives the wrapped query less importance, above one more.
/// More complex boosts need a function score query, which this port does not have
/// yet.
export class BoostQuery final : public Query {
public:
    /// A boost of `boost` on `query`; the boost must be finite and non-negative,
    /// and `-0.0f` is rejected.
    [[nodiscard]] static Result<BoostQuery> create(std::shared_ptr<Query> query, float boost);

    /// The wrapped query.
    [[nodiscard]] const std::shared_ptr<Query> &query() const noexcept { return query_; }
    /// The boost factor.
    [[nodiscard]] float boost() const noexcept { return boost_; }

    // ---- Query ----

    /// The wrapped query's weight, with the boosts multiplied.
    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    /// Rewrites the wrapped query and folds the boosts; an unchanged query
    /// answers a null pointer.
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    /// **Deviation:** the reference implementation visits the wrapped query
    /// through a sub visitor for the `MUST` occurrence; the port has no
    /// `get_sub_visitor` yet, so the visitor is forwarded unchanged.
    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `(inner)^boost`, with the boost printed with one decimal, the form the
    /// reference implementation prints.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    BoostQuery(std::shared_ptr<Query> query, float boost);

    std::shared_ptr<Query> query_;
    float boost_{1.0F};
};

} // namespace lucene
