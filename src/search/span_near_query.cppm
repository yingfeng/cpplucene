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

export module lucene_core:span_near_query;

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

export class SpanNearQuery;

/// The weight of a [`SpanNearQuery`].
///
/// Port of `SpanNearQuery.SpanNearWeight`.
export class SpanNearWeight final : public SpanWeight {
public:
    SpanNearWeight(const SpanNearQuery &query, std::vector<std::unique_ptr<SpanWeight>> sub_weights,
                   const IndexSearcher &searcher, TermStatesMap term_states, float boost);

    [[nodiscard]] VoidResult extract_term_states(TermStatesMap &contexts) const override;

    [[nodiscard]] Result<std::unique_ptr<Spans>> get_spans(const LeafReaderContext &context,
                                                           SpanPostings required) override;

private:
    std::vector<std::unique_ptr<SpanWeight>> sub_weights_;
};

/// Matches documents where the sub queries' spans appear close together.
///
/// Port of `SpanNearQuery` (`queries/span/span_near_query.rs`).  With
/// `in_order` the sub spans must appear in the order of the clauses and the gaps
/// between them may sum to at most `slop`; the slop is counted in positions
/// between the spans, so `slop = 0` means "adjacent" and a negative value allows
/// that much overlap.
///
/// With `in_order` false the sub spans may appear in any order and the slop is
/// the number of positions between them that no sub span covers
/// ([`NearSpansUnordered`](lucene_core:near_spans_unordered)).
export class SpanNearQuery final : public SpanQuery {
public:
    /// `clauses` must be non-empty and all of them span queries on the same field.
    /// A negative `slop` is allowed: it lets the spans overlap by that many
    /// positions, which is what [`FieldMaskingSpanQuery`] combines with to match
    /// terms that sit at the same position of different fields.
    [[nodiscard]] static Result<SpanNearQuery> create(std::vector<std::shared_ptr<SpanQuery>> clauses, int32_t slop,
                                                      bool in_order);

    [[nodiscard]] const std::vector<std::shared_ptr<SpanQuery>> &clauses() const noexcept { return clauses_; }
    [[nodiscard]] int32_t slop() const noexcept { return slop_; }
    [[nodiscard]] bool in_order() const noexcept { return in_order_; }

    [[nodiscard]] std::string_view get_field() const override { return clauses_.front()->get_field(); }

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                               float boost) const override;

    /// Rewrites the clauses and rebuilds the query when one of them changed.
    ///
    /// **Deviation:** the reference implementation inherits the default (which
    /// returns the query unchanged), so a
    /// [`SpanMultiTermQueryWrapper`](lucene_core:span_multi_term_query_wrapper)
    /// nested in a near query keeps failing with "rewrite first".  Rewriting is
    /// semantics-preserving, so doing it here only makes such queries usable.
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    SpanNearQuery(std::vector<std::shared_ptr<SpanQuery>> clauses, int32_t slop, bool in_order);

    std::vector<std::shared_ptr<SpanQuery>> clauses_;
    int32_t slop_;
    bool in_order_;
};

} // namespace lucene
