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

export module lucene_core:span_or_query;

import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :span_query;
import :span_weight;
import :spans;
import :types;
import :weight;

import std;

namespace lucene {

export class SpanOrQuery;

/// The weight of a [`SpanOrQuery`].
///
/// Port of `SpanOrQuery.SpanOrWeight`.
export class SpanOrWeight final : public SpanWeight {
public:
    SpanOrWeight(const SpanOrQuery &query, std::vector<std::unique_ptr<SpanWeight>> sub_weights,
                 const IndexSearcher &searcher, TermStatesMap term_states, float boost);

    [[nodiscard]] VoidResult extract_term_states(TermStatesMap &contexts) const override;

    [[nodiscard]] Result<std::unique_ptr<Spans>> get_spans(const LeafReaderContext &context,
                                                           SpanPostings required) override;

private:
    std::vector<std::unique_ptr<SpanWeight>> sub_weights_;
};

/// Matches the union of the positions of its clauses.
///
/// Port of `SpanOrQuery` (`queries/span/span_or_query.rs`).  A document matches
/// when at least one clause does, and the positions of all clauses are merged in
/// increasing `(start, end)` order.
///
/// **Deviation:** the reference implementation merges the positions through a
/// priority queue of `SpanDisiWrapper`s; this port keeps one cursor per clause and
/// picks the smallest on every step.  The enumeration order and the resulting
/// span positions are the same, only the constant factor of one step differs
/// (clause count is small in practice).  The two-phase view is not published, for
/// the same reason as in [`SpanScorer`](lucene_core:span_scorer).
export class SpanOrQuery final : public SpanQuery {
public:
    /// `clauses` must be non-empty and all of them span queries on the same field.
    [[nodiscard]] static Result<SpanOrQuery> create(std::vector<std::shared_ptr<SpanQuery>> clauses);

    [[nodiscard]] const std::vector<std::shared_ptr<SpanQuery>> &clauses() const noexcept { return clauses_; }

    [[nodiscard]] std::string_view get_field() const override { return clauses_.front()->get_field(); }

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                               float boost) const override;

    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    explicit SpanOrQuery(std::vector<std::shared_ptr<SpanQuery>> clauses);

    std::vector<std::shared_ptr<SpanQuery>> clauses_;
};

} // namespace lucene
