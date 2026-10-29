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

export module lucene_core:span_not_query;

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

export class SpanNotQuery;

/// The weight of a [`SpanNotQuery`].
///
/// Port of `SpanNotQuery.SpanNotWeight`.
export class SpanNotWeight final : public SpanWeight {
public:
    SpanNotWeight(const SpanNotQuery &query, std::unique_ptr<SpanWeight> include_weight,
                  std::unique_ptr<SpanWeight> exclude_weight, const IndexSearcher &searcher, TermStatesMap term_states,
                  float boost);

    [[nodiscard]] VoidResult extract_term_states(TermStatesMap &contexts) const override;

    [[nodiscard]] Result<std::unique_ptr<Spans>> get_spans(const LeafReaderContext &context,
                                                           SpanPostings required) override;

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override;

private:
    std::unique_ptr<SpanWeight> include_weight_;
    std::unique_ptr<SpanWeight> exclude_weight_;
};

/// Accepts the spans of `include` that do not overlap the spans of `exclude`
/// within `pre` positions before or `post` positions after them.
///
/// Port of `SpanNotQuery` (`queries/span/span_not_query.rs`).  Negative `pre` /
/// `post` allow a certain amount of overlap, exactly like the reference
/// implementation.
export class SpanNotQuery final : public SpanQuery {
public:
    [[nodiscard]] static Result<SpanNotQuery> create(std::shared_ptr<SpanQuery> include,
                                                     std::shared_ptr<SpanQuery> exclude, int32_t pre, int32_t post);

    [[nodiscard]] const std::shared_ptr<SpanQuery> &include() const noexcept { return include_; }
    [[nodiscard]] const std::shared_ptr<SpanQuery> &exclude() const noexcept { return exclude_; }
    [[nodiscard]] int32_t pre() const noexcept { return pre_; }
    [[nodiscard]] int32_t post() const noexcept { return post_; }

    [[nodiscard]] std::string_view get_field() const override { return include_->get_field(); }

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                               float boost) const override;

    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    SpanNotQuery(std::shared_ptr<SpanQuery> include, std::shared_ptr<SpanQuery> exclude, int32_t pre, int32_t post);

    std::shared_ptr<SpanQuery> include_;
    std::shared_ptr<SpanQuery> exclude_;
    int32_t pre_;
    int32_t post_;
};

} // namespace lucene
