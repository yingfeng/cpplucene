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

export module lucene_core:range_field_query;

import :types;
import std;

import :error;
import :point_values;
import :query;
import :query_visitor;
import :score_mode;
import :weight;

namespace lucene {

export class IndexSearcher;

/// The relation a range query is looking for.
///
/// Port of `QueryType` (`core/document/range_field_query.rs`).
export enum class QueryType : int32_t {
    /// Ranges that intersect the query range (overlap, within or contains).
    Intersects = 0,
    /// Ranges that lie within the query range.
    Within = 1,
    /// Ranges that contain the query range.
    Contains = 2,
    /// Ranges that are neither disjoint from nor within the query range.
    Crosses = 3,
};

/// How [`RangeFieldQuery::to_string`] renders one dimension of the encoded
/// ranges.
///
/// Port of the `RangeFieldQueryBase` trait: the concrete range field types
/// (`IntRange`, `LongRange`, ...) supply the decoder, so the query itself stays
/// agnostic of the encoding.
export class RangeFieldFormat {
public:
    virtual ~RangeFieldFormat() = default;

    /// Renders dimension `dimension` of `ranges` (the same bytes the query holds).
    [[nodiscard]] virtual Result<std::string> format(std::span<const uint8_t> ranges, size_t dimension) const = 0;
};

/// How the encoded range `[min_packed_value, max_packed_value]` relates to the
/// query range.
///
/// Port of `QueryType::compare`.  The reference implementation threads a
/// `ByteArrayComparator` through every call; this port compares the bytes of a
/// dimension unsigned, which is what the comparator does for the sortable
/// encodings the range fields produce.
[[nodiscard]] Result<Relation> range_field_query_compare(QueryType query_type, std::span<const uint8_t> query,
                                                         std::span<const uint8_t> min_packed_value,
                                                         std::span<const uint8_t> max_packed_value, size_t num_dims,
                                                         size_t bytes_per_dim);

/// Whether `packed_value` satisfies the relation of the query.
///
/// Port of `QueryType::matches`.
[[nodiscard]] Result<bool> range_field_query_matches(QueryType query_type, std::span<const uint8_t> query,
                                                     std::span<const uint8_t> packed_value, size_t num_dims,
                                                     size_t bytes_per_dim);

/// Matches the indexed ranges of a range field by a relation.
///
/// Port of `RangeFieldQuery` (`core/document/range_field_query.rs`).
///
/// `ranges` holds the sortable encoding of `num_dims` `[min, max]` pairs: the
/// minimum of every dimension first, then the maximum of every dimension, which
/// is why one dimension takes `bytes_per_dim` bytes and the whole encoding
/// `2 * num_dims * bytes_per_dim`.
///
/// **Deviation:** the reference implementation collects the matching documents in a
/// `DocIdSetBuilder` while the scorer supplier is resolved lazily; this port
/// collects them in a set up front, like [`PointRangeQuery`], which keeps the
/// scorer a plain constant-score one.
export class RangeFieldQuery final : public Query {
public:
    /// The encoding of [`to_string`] is `format`'s job; it must not be null.
    [[nodiscard]] static Result<RangeFieldQuery> create(std::string field, std::vector<uint8_t> ranges, size_t num_dims,
                                                        QueryType query_type,
                                                        std::shared_ptr<const RangeFieldFormat> format);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] const std::vector<uint8_t> &ranges() const noexcept { return ranges_; }
    [[nodiscard]] size_t num_dims() const noexcept { return num_dims_; }
    [[nodiscard]] size_t bytes_per_dim() const noexcept { return bytes_per_dim_; }
    [[nodiscard]] QueryType query_type() const noexcept { return query_type_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                               float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    RangeFieldQuery(std::string field, std::vector<uint8_t> ranges, size_t num_dims, QueryType query_type,
                    std::shared_ptr<const RangeFieldFormat> format);

    std::string field_;
    std::vector<uint8_t> ranges_;
    size_t num_dims_;
    size_t bytes_per_dim_;
    QueryType query_type_;
    std::shared_ptr<const RangeFieldFormat> format_;
};

} // namespace lucene
