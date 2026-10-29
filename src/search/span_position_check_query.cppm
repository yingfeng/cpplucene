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

export module lucene_core:span_position_check_query;

import :filter_spans;
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

export class SpanPositionCheckQuery;

/// The weight of a [`SpanPositionCheckQuery`]: the spans of the wrapped query,
/// filtered by the query's position check.
///
/// Port of `SpanPositionCheckQuery.SpanPositionCheckWeight`.
export class SpanPositionCheckWeight final : public SpanWeight {
public:
    SpanPositionCheckWeight(const SpanPositionCheckQuery &query, std::unique_ptr<SpanWeight> match_weight,
                            const IndexSearcher &searcher, TermStatesMap term_states, float boost);

    [[nodiscard]] VoidResult extract_term_states(TermStatesMap &contexts) const override;

    [[nodiscard]] Result<std::unique_ptr<Spans>> get_spans(const LeafReaderContext &context,
                                                           SpanPostings required) override;

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override;

private:
    std::unique_ptr<SpanWeight> match_weight_;
};

/// The base of the span queries that keep or drop a wrapped query's spans by
/// looking at their position.
///
/// Port of `SpanPositionCheckQuery` (`queries/span/span_position_check_query.rs`).
/// The wrapped spans are run through a [`FilterSpans`](lucene_core:filter_spans)
/// whose decision is [`accept_position`].
export class SpanPositionCheckQuery : public SpanQuery {
public:
    /// The wrapped query, whose spans are filtered.
    [[nodiscard]] const std::shared_ptr<SpanQuery> &match() const noexcept { return match_; }

    [[nodiscard]] std::string_view get_field() const override { return match_->get_field(); }

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                               float boost) const override;

    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

    /// Called with the wrapped spans positioned at a candidate; decides whether it
    /// is kept, dropped, or ends the current document.
    ///
    /// **Deviation:** public, because the filtering spans lives in another
    /// translation unit and C++ has no package-private access.
    [[nodiscard]] virtual Result<SpanAcceptStatus> accept_position(Spans &spans) const = 0;

protected:
    /// A copy of this query wrapping `match`; used by [`rewrite`].
    ///
    /// **Deviation:** the reference implementation clones itself and swaps the
    /// wrapped query; C++ has no `Cloneable`, so every subclass supplies the copy.
    [[nodiscard]] virtual std::shared_ptr<SpanQuery> with_match(std::shared_ptr<SpanQuery> match) const = 0;

    explicit SpanPositionCheckQuery(std::shared_ptr<SpanQuery> match);

    std::shared_ptr<SpanQuery> match_;
};

} // namespace lucene
