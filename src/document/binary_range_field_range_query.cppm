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

export module lucene_core:binary_range_field_range_query;

import :types;
import std;

import :error;
import :query;
import :query_visitor;
import :range_field_query;
import :score_mode;
import :weight;

namespace lucene {

export class IndexSearcher;

/// Matches encoded binary doc values ranges without a point index: the "slow"
/// path of the range fields.
///
/// Port of `BinaryRangeFieldRangeQuery`
/// (`core/document/binary_range_field_range_query.rs`).
///
/// Only [`QueryType::Intersects`] is supported, exactly like the reference
/// implementation.
///
/// **Deviation:** the reference implementation keeps a variant enum of the four
/// concrete slow range queries to render itself and compare for equality; this port
/// keeps the field and the already rendered range text, which is all those methods
/// need.
export class BinaryRangeFieldRangeQuery final : public Query {
public:
    [[nodiscard]] static Result<BinaryRangeFieldRangeQuery>
    create(std::string field, std::vector<uint8_t> query_packed_value, size_t num_bytes_per_dimension, size_t num_dims,
           QueryType query_type, std::string range_text);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] const std::vector<uint8_t> &query_packed_value() const noexcept { return query_packed_value_; }
    [[nodiscard]] size_t num_dims() const noexcept { return num_dims_; }
    [[nodiscard]] size_t num_bytes_per_dimension() const noexcept { return num_bytes_per_dimension_; }
    [[nodiscard]] QueryType query_type() const noexcept { return query_type_; }
    /// How the query renders its bounds, e.g. `[[10, 20] TO [30, 40]]`.
    [[nodiscard]] const std::string &range_text() const noexcept { return range_text_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                               float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    BinaryRangeFieldRangeQuery(std::string field, std::vector<uint8_t> query_packed_value,
                               size_t num_bytes_per_dimension, size_t num_dims, QueryType query_type,
                               std::string range_text);

    std::string field_;
    std::vector<uint8_t> query_packed_value_;
    size_t num_dims_;
    size_t num_bytes_per_dimension_;
    QueryType query_type_;
    std::string range_text_;
};

} // namespace lucene
