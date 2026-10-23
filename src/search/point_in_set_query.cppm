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

export module lucene_core:point_in_set_query;

import :types;
import std;

import :error;
import :explanation;
import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :weight;

namespace lucene {

export class PointInSetQuery;

/// Matches the documents whose point value is one of a set of points.
///
/// Port of `PointInSetQuery` (`core/search/point_in_set_query.rs`).
///
/// **Deviations:** the reference implementation keeps a cursor into the sorted
/// query points while it walks the tree and prints the points through a per-type
/// formatter; the port binary-searches the set for every cell and prints the
/// points as hexadecimal bytes.
export class PointInSetQuery final : public Query {
public:
    /// A query over `field` matching any of `points`, which hold `num_dims`
    /// fixed-width values each, most significant byte first.
    [[nodiscard]] static Result<PointInSetQuery> create(std::string field,
                                                        std::vector<std::vector<uint8_t>> points, size_t num_dims);

    /// The field the query reads.
    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    /// How many dimensions the points have.
    [[nodiscard]] size_t num_dims() const noexcept { return num_dims_; }
    /// How many bytes one dimension takes.
    [[nodiscard]] size_t bytes_per_dim() const noexcept { return bytes_per_dim_; }
    /// The accepted points, sorted byte-wise and free of duplicates.
    [[nodiscard]] const std::vector<std::vector<uint8_t>> &points() const noexcept { return points_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `field:{<hex>,<hex>}`.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    PointInSetQuery(std::string field, std::vector<std::vector<uint8_t>> points, size_t num_dims);

    std::string field_;
    std::vector<std::vector<uint8_t>> points_;
    size_t num_dims_{1};
    size_t bytes_per_dim_{0};
};

/// The weight of a [`PointInSetQuery`].
///
/// Port of `PointInSetQueryWeight` (`core/search/point_in_set_query.rs`).
export class PointInSetWeight final : public Weight {
public:
    /// The weight of `query`, with the given score and score mode.
    PointInSetWeight(const PointInSetQuery &query, float score, ScoreMode score_mode);

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override;

    [[nodiscard]] const Query &get_query() const override { return query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher &searcher) override;

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    [[nodiscard]] Result<std::unique_ptr<Scorer>> scorer_for(const LeafReaderContext &context) const;

    const PointInSetQuery &query_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::Complete};
};

} // namespace lucene
