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

export module lucene_core:boolean_query;

import :types;

import std;

import :boolean_clause;
import :error;
import :query;
import :score_mode;
import :weight;

namespace lucene {

export class IndexSearcher;

/// A query that matches documents matching boolean combinations of other
/// queries.
///
/// Port of `BooleanQuery` (`core/search/boolean_query.rs`).
///
/// A clause is `MUST` (must match, scores), `FILTER` (must match, does not
/// score), `SHOULD` (may match, scores) or `MUST_NOT` (must not match).  When
/// there is no `MUST`/`FILTER` clause, at least
/// [`minimum_number_should_match`] `SHOULD` clauses have to match.
///
/// **Deviations** (C++ mechanics only):
///  * `FILTER`/`MUST_NOT` duplicates are dropped by [`equals`] like the reference implementation does
///    with its enum `PartialEq`, but the deduplication happens when the clause
///    sets are built rather than through a `HashSet`;
///  * the single-clause `FILTER` rewrite into `Boost(ConstantScore(...), 0)` and
///    the `ConstantScoreQuery` extra simplifications in [`rewrite`] are not
///    ported, because those queries are not ported; the clause is kept as it is
///    (the supplier already scores it 0);
///  * [`visit`] recurses into every clause instead of asking the visitor for a
///    sub-visitor with an `Occur`, which `QueryVisitor` does not have yet.
export class BooleanQuery final : public Query {
public:
    /// Incrementally builds a [`BooleanQuery`].
    class Builder {
    public:
        Builder &set_minimum_number_should_match(int32_t min) noexcept;

        /// Adds a clause; fails when the clause limit is reached.
        [[nodiscard]] VoidResult add_clause(BooleanClause clause);

        /// Adds `query` with `occur`.
        [[nodiscard]] VoidResult add(std::shared_ptr<Query> query, Occur occur);

        [[nodiscard]] Result<BooleanQuery> build();

    private:
        int32_t min_should_match_{0};
        std::vector<BooleanClause> clauses_;
    };

    /// The default limit on the number of clauses, `get_max_clause_count()` in
    /// The reference implementation (which allows it to be configured globally; the port keeps the
    /// default).
    static constexpr size_t DEFAULT_MAX_CLAUSE_COUNT = 1024;

    [[nodiscard]] static constexpr size_t max_clause_count() noexcept { return DEFAULT_MAX_CLAUSE_COUNT; }

    /// Builds a query directly; `min_should_match` must not be negative.
    [[nodiscard]] static Result<BooleanQuery> create(std::vector<BooleanClause> clauses, int32_t min_should_match = 0);

    /// How many `SHOULD` clauses have to match when no `MUST`/`FILTER` clause
    /// exists.
    [[nodiscard]] int32_t minimum_number_should_match() const noexcept { return min_should_match_; }

    /// The clauses, in the order they were added.
    [[nodiscard]] const std::vector<BooleanClause> &clauses() const noexcept { return clauses_; }

    /// The indices of the clauses with `occur`.
    [[nodiscard]] const std::vector<size_t> &clause_indices(Occur occur) const noexcept {
        return clause_sets_[static_cast<size_t>(occur)];
    }

    /// Whether this is a pure disjunction: only `SHOULD` clauses, at most one of
    /// which has to match.
    [[nodiscard]] bool is_pure_disjunction() const noexcept;

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    BooleanQuery(std::vector<BooleanClause> clauses, int32_t min_should_match);

    std::vector<BooleanClause> clauses_;
    std::array<std::vector<size_t>, 4> clause_sets_;
    int32_t min_should_match_;
};

} // namespace lucene
