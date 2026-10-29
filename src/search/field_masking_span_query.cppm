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

export module lucene_core:field_masking_span_query;

import :index_searcher;
import :query;
import :query_visitor;
import :score_mode;
import :span_query;
import :types;
import :weight;

import std;

namespace lucene {

/// Presents another span query under a different field name, so that span queries
/// can combine clauses that live in different fields.
///
/// Port of `FieldMaskingSpanQuery` (`queries/span/field_masking_span_query.rs`).
/// The wrapped query still reads its own postings; only the field name the
/// surrounding span query sees is replaced, which is what lets `SpanNearQuery` /
/// `SpanOrQuery` (that require one field) span several columns of the same
/// document.
///
/// The weight of this query **is** the weight of the wrapped query, exactly like
/// the reference implementation, so scoring uses the wrapped query's field
/// statistics.
export class FieldMaskingSpanQuery final : public SpanQuery {
public:
    FieldMaskingSpanQuery(std::shared_ptr<SpanQuery> masked_query, std::string masked_field);

    [[nodiscard]] const std::shared_ptr<SpanQuery> &masked_query() const noexcept { return masked_query_; }

    /// The field name supplied here, not the wrapped query's.
    [[nodiscard]] std::string_view get_field() const override { return field_; }

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                               float boost) const override;

    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    std::shared_ptr<SpanQuery> masked_query_;
    std::string field_;
};

} // namespace lucene
