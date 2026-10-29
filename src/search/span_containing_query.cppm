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

export module lucene_core:span_containing_query;

import :index_searcher;
import :leaf_reader_context;
import :query;
import :span_contain_query;
import :span_query;
import :span_weight;
import :spans;
import :types;

import std;

namespace lucene {

export class SpanContainingQuery;

/// The weight of a [`SpanContainingQuery`]: the `big` spans that contain at least
/// one `little` span.
///
/// Port of `SpanContainingQuery.SpanContainingWeight`.
export class SpanContainingWeight final : public SpanContainWeight {
public:
    SpanContainingWeight(const SpanContainingQuery &query, std::unique_ptr<SpanWeight> big_weight,
                         std::unique_ptr<SpanWeight> little_weight, const IndexSearcher &searcher,
                         TermStatesMap term_states, float boost);

    [[nodiscard]] Result<std::unique_ptr<Spans>> get_spans(const LeafReaderContext &context,
                                                           SpanPostings required) override;
};

/// Keeps the spans of `big` that contain at least one span of `little`.
///
/// Port of `SpanContainingQuery` (`queries/span/span_containing_query.rs`).  The
/// reported match (and its payload) is the `big` one.
export class SpanContainingQuery final : public SpanContainQuery {
public:
    /// `big` and `little` must be on the same field.
    [[nodiscard]] static Result<SpanContainingQuery> create(std::shared_ptr<SpanQuery> big,
                                                            std::shared_ptr<SpanQuery> little);

    /// Prefer [`create`], which validates the operands; this constructor is public
    /// because [`with_parts`] has to build one and C++ cannot grant it access.
    SpanContainingQuery(std::shared_ptr<SpanQuery> big, std::shared_ptr<SpanQuery> little);

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

protected:
    [[nodiscard]] std::shared_ptr<SpanQuery> with_parts(std::shared_ptr<SpanQuery> big,
                                                        std::shared_ptr<SpanQuery> little) const override;

    [[nodiscard]] std::unique_ptr<SpanWeight> build_weight(std::unique_ptr<SpanWeight> big_weight,
                                                           std::unique_ptr<SpanWeight> little_weight,
                                                           const IndexSearcher &searcher, TermStatesMap term_states,
                                                           float boost) const override;

};

} // namespace lucene
