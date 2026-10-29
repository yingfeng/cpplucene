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

export module lucene_core:span_term_query;

import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :span_query;
import :span_weight;
import :spans;
import :term;
import :term_states;
import :types;
import :weight;

import std;

namespace lucene {

export class SpanTermQuery;

/// The weight of a [`SpanTermQuery`]: one term's spans, one position per
/// occurrence.
///
/// Port of `SpanTermQuery.SpanTermWeight`.
export class SpanTermWeight final : public SpanWeight {
public:
    SpanTermWeight(const SpanTermQuery &query, const IndexSearcher &searcher, TermStatesMap term_states, float boost);

    [[nodiscard]] VoidResult extract_term_states(TermStatesMap &contexts) const override;

    [[nodiscard]] Result<std::unique_ptr<Spans>> get_spans(const LeafReaderContext &context,
                                                           SpanPostings required) override;

private:
    Term term_;
    std::shared_ptr<TermStates> term_states_;
};

/// Matches the positions of a single term.
///
/// Port of `SpanTermQuery` (`queries/span/span_term_query.rs`).  The field must
/// be indexed with positions, otherwise the weight reports `illegal_state` (the
/// reference implementation throws `IllegalStateException`).
export class SpanTermQuery final : public SpanQuery {
public:
    explicit SpanTermQuery(Term term);

    [[nodiscard]] const Term &term() const noexcept { return term_; }

    [[nodiscard]] std::string_view get_field() const override { return term_.field(); }

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                               float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    Term term_;
};

} // namespace lucene
