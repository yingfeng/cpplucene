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

export module lucene_core:point_range_query;

import :types;
import std;

import :error;
import :explanation;
import :index_searcher;
import :leaf_reader_context;
import :point_values;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :weight;

namespace lucene {

export class PointRangeQuery;

/// Matches the documents whose point value lies inside a range, per dimension.
///
/// Port of `PointRangeQuery` (`core/search/point_range_query.rs`).
///
/// Every match scores the same (the boost), so the query is a constant score one.
///
/// **Deviation:** the reference implementation formats the endpoints through a
/// per-point-type `sub` formatter (`IntPoint`, `LongPoint`, ...), which the port
/// does not have; [`to_string`] therefore prints the endpoints as hexadecimal
/// bytes.
export class PointRangeQuery final : public Query {
public:
    /// A query over `field` whose points lie in `[lower_point, upper_point]`.
    /// Both endpoints hold `num_dims` fixed-width values, most significant byte
    /// first.
    [[nodiscard]] static Result<PointRangeQuery> create(std::string field, std::vector<uint8_t> lower_point,
                                                        std::vector<uint8_t> upper_point, size_t num_dims);

    /// The field the query reads.
    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    /// How many dimensions the points have.
    [[nodiscard]] size_t num_dims() const noexcept { return num_dims_; }
    /// How many bytes one dimension takes.
    [[nodiscard]] size_t bytes_per_dim() const noexcept { return bytes_per_dim_; }
    /// The inclusive lower endpoint.
    [[nodiscard]] const std::vector<uint8_t> &lower_point() const noexcept { return lower_point_; }
    /// The inclusive upper endpoint.
    [[nodiscard]] const std::vector<uint8_t> &upper_point() const noexcept { return upper_point_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `field:[lo TO hi]`, with one bracket pair per dimension.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    PointRangeQuery(std::string field, std::vector<uint8_t> lower_point, std::vector<uint8_t> upper_point,
                    size_t num_dims);

    std::string field_;
    std::vector<uint8_t> lower_point_;
    std::vector<uint8_t> upper_point_;
    size_t num_dims_{1};
    size_t bytes_per_dim_{0};
};

/// The weight of a [`PointRangeQuery`]: it intersects the points of a leaf and
/// hands the matching documents to a constant score scorer.
///
/// Port of `PointRangeWeight` (`core/search/point_range_query.rs`).
///
/// **Deviation:** the reference implementation collects into a `DocIdSetBuilder`
/// and estimates the hit count from the point tree; here the matching documents
/// go into a sorted set, which keeps the code small at the cost of an
/// allocation per leaf.
export class PointRangeWeight final : public Weight {
public:
    /// The weight of `query`, with the given score and score mode.
    PointRangeWeight(const PointRangeQuery &query, float score, ScoreMode score_mode);

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override;

    [[nodiscard]] const Query &get_query() const override { return query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher &searcher) override;

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    /// The scorer of one leaf, or a null pointer when the leaf has no points for
    /// the field.
    [[nodiscard]] Result<std::unique_ptr<Scorer>> scorer_for(const LeafReaderContext &context) const;

    const PointRangeQuery &query_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::Complete};
};

} // namespace lucene
