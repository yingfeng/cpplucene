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

export module lucene_core:disjunction_max_query;

import :types;
import std;

import :error;
import :query;
import :query_visitor;
import :score_mode;
import :weight;

namespace lucene {

/// A query that generates the union of the documents produced by its
/// sub-queries and scores each document with the maximum score of the
/// sub-queries that match it, plus a tie breaking increment for the other
/// matching sub-queries.
///
/// Port of `DisjunctionMaxQuery` (`core/search/disjunction_max_query.rs`).  This
/// is what a "search the same word in several fields with different boosts"
/// query needs: the primary score is the best field, not the sum over the
/// fields, and a small `tie_breaker_multiplier` (on the order of `0.1`) lets a
/// term that occurs in several fields beat one that occurs only in the best
/// field.
///
/// **Deviations:**
///  * the reference implementation keeps a `HashMap<Query, usize>` multiset of
///    the disjuncts for equality and visiting, and an ordered vector for
///    scoring; the port keeps the ordered vector only and compares/visits the
///    disjuncts in that order (equality stays order-insensitive);
///  * `DisjunctionMaxBulkScorer` (the top-scores fast path) is not ported, so
///    the weight always goes through the default bulk scorer;
///  * `Weight::matches` is not ported (it needs `QueryWeightMatches`).
export class DisjunctionMaxQuery final : public Query {
public:
    /// Creates the query from its disjuncts.
    ///
    /// `tie_breaker_multiplier` has to be in `[0, 1]`; the value should be small
    /// when it is not zero.
    [[nodiscard]] static Result<std::shared_ptr<DisjunctionMaxQuery>>
    create(std::vector<std::shared_ptr<Query>> disjuncts, float tie_breaker_multiplier);

    /// The disjuncts, in the order they were given.
    [[nodiscard]] const std::vector<std::shared_ptr<Query>> &disjuncts() const noexcept { return disjuncts_; }

    /// How strongly the non-maximum sub-scores count.
    [[nodiscard]] float tie_breaker_multiplier() const noexcept { return tie_breaker_multiplier_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    DisjunctionMaxQuery(std::vector<std::shared_ptr<Query>> disjuncts, float tie_breaker_multiplier);

    std::vector<std::shared_ptr<Query>> disjuncts_;
    float tie_breaker_multiplier_;
};

} // namespace lucene
