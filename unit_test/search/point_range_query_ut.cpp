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

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.point_range_query;

import lucene_core;

using namespace lucene;

namespace {

/// Point values over a map from document to packed value, intersected by brute
/// force but through the same visitor protocol a BKD tree uses.
class MapPointValues final : public PointValues {
public:
    MapPointValues(std::map<int32_t, std::vector<uint8_t>> points, size_t num_dims)
            : points_(std::move(points)), num_dims_(num_dims) {}

    [[nodiscard]] Result<std::optional<BytesRef>> get_min_packed_value() const override {
        return std::optional<BytesRef>{};
    }
    [[nodiscard]] Result<std::optional<BytesRef>> get_max_packed_value() const override {
        return std::optional<BytesRef>{};
    }
    [[nodiscard]] Result<size_t> get_num_dimensions() const override { return num_dims_; }
    [[nodiscard]] Result<size_t> get_num_index_dimensions() const override { return num_dims_; }
    [[nodiscard]] Result<size_t> get_bytes_per_dimension() const override {
        return points_.empty() ? size_t{0} : points_.begin()->second.size() / num_dims_;
    }
    [[nodiscard]] Result<size_t> size() const override { return points_.size(); }
    [[nodiscard]] Result<int32_t> get_doc_count() const override {
        return static_cast<int32_t>(points_.size());
    }
    [[nodiscard]] Result<std::unique_ptr<PointTree>> get_point_tree() const override {
        return std::unique_ptr<PointTree>{};
    }

    [[nodiscard]] VoidResult intersect(IntersectVisitor &visitor) const override {
        for (const auto &[doc, value] : points_) {
            auto relation = visitor.compare(value, value);
            if (!relation.has_value()) {
                return std::unexpected(std::move(relation.error()));
            }
            if (relation.value() == Relation::kCellInsideQuery) {
                if (auto visited = visitor.visit(doc); !visited.has_value()) {
                    return std::unexpected(std::move(visited.error()));
                }
            } else if (relation.value() == Relation::kCellCrossesQuery) {
                if (auto visited = visitor.visit_with_packed_value(doc, value); !visited.has_value()) {
                    return std::unexpected(std::move(visited.error()));
                }
            }
        }
        return {};
    }

private:
    std::map<int32_t, std::vector<uint8_t>> points_;
    size_t num_dims_{1};
};

/// A leaf whose only data is one points field.
class PointsLeafReader final : public LeafReader {
public:
    explicit PointsLeafReader(std::shared_ptr<PointValues> points) : points_(std::move(points)) {}

    [[nodiscard]] Result<int32_t> max_doc() const override { return 16; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return 16; }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view /*field*/) override {
        return std::unique_ptr<Terms>{};
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric_doc_values(
            std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set_doc_values(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::shared_ptr<PointValues>> point_values(std::string_view field) override {
        if (field != "num") {
            return std::shared_ptr<PointValues>{};
        }
        return points_;
    }
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    std::shared_ptr<PointValues> points_;
    FieldInfos field_infos_{FieldInfos::none()};
};

IndexSearcher make_searcher(std::shared_ptr<PointValues> points) {
    std::shared_ptr<IndexReader> reader = std::make_shared<PointsLeafReader>(std::move(points));
    return IndexSearcher(std::move(reader));
}

std::vector<int32_t> matching_docs(const Query &query, const IndexSearcher &searcher) {
    std::vector<int32_t> docs;
    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    EXPECT_TRUE(weight.has_value()) << weight.error().to_string();
    if (!weight.has_value()) {
        return docs;
    }
    auto leaves = searcher.leaves();
    EXPECT_TRUE(leaves.has_value());
    if (!leaves.has_value()) {
        return docs;
    }
    for (const LeafReaderContext &context : leaves.value()) {
        auto scorer = weight.value()->scorer(context, searcher);
        EXPECT_TRUE(scorer.has_value()) << scorer.error().to_string();
        if (!scorer.has_value() || scorer.value() == nullptr) {
            continue;
        }
        auto doc = scorer.value()->next_doc();
        while (doc.has_value() && doc.value() != NO_MORE_DOCS) {
            docs.push_back(doc.value());
            doc = scorer.value()->next_doc();
        }
    }
    return docs;
}

} // namespace

TEST(PointRangeQueryTest, MatchesTheValuesInsideTheRange) {
    // One byte per dimension, one dimension: 1, 3, 5, 7.
    std::map<int32_t, std::vector<uint8_t>> points{{0, {1}}, {1, {3}}, {2, {5}}, {3, {7}}};
    const IndexSearcher searcher = make_searcher(std::make_shared<MapPointValues>(std::move(points), 1));

    auto query = PointRangeQuery::create("num", {3}, {5}, 1);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{1, 2}));
}

TEST(PointRangeQueryTest, TheRangeIncludesBothEndpoints) {
    std::map<int32_t, std::vector<uint8_t>> points{{0, {1}}, {1, {3}}, {2, {5}}, {3, {7}}};
    const IndexSearcher searcher = make_searcher(std::make_shared<MapPointValues>(std::move(points), 1));

    auto single = PointRangeQuery::create("num", {3}, {3}, 1);
    ASSERT_TRUE(single.has_value()) << single.error().to_string();
    EXPECT_EQ(matching_docs(single.value(), searcher), (std::vector<int32_t>{1}));
}

TEST(PointRangeQueryTest, TwoDimensionsHaveToBothBeInside) {
    // (1,1), (1,5), (5,5)
    std::map<int32_t, std::vector<uint8_t>> points{{0, {1, 1}}, {1, {1, 5}}, {2, {5, 5}}};
    const IndexSearcher searcher = make_searcher(std::make_shared<MapPointValues>(std::move(points), 2));

    auto query = PointRangeQuery::create("num", {1, 4}, {6, 6}, 2);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{1, 2}));
}

TEST(PointRangeQueryTest, AFieldWithoutPointsMatchesNothing) {
    std::map<int32_t, std::vector<uint8_t>> points{{0, {1}}};
    const IndexSearcher searcher = make_searcher(std::make_shared<MapPointValues>(std::move(points), 1));

    auto query = PointRangeQuery::create("other", {0}, {9}, 1);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_TRUE(matching_docs(query.value(), searcher).empty());
}

TEST(PointRangeQueryTest, MismatchedDimensionsAreRejected) {
    // Indexed with one dimension, queried with two.
    std::map<int32_t, std::vector<uint8_t>> points{{0, {1}}};
    const IndexSearcher searcher = make_searcher(std::make_shared<MapPointValues>(std::move(points), 1));

    auto query = PointRangeQuery::create("num", {0, 0}, {9, 9}, 2);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    auto weight = query.value().create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());
    auto supplier = weight.value()->scorer_supplier(leaves.value()[0], searcher);
    ASSERT_FALSE(supplier.has_value());
    EXPECT_EQ(supplier.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(PointRangeQueryTest, ToStringPrintsHexadecimalEndpoints) {
    auto query = PointRangeQuery::create("num", {3}, {5}, 1);
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(query.value().to_string(""), "num:[03 TO 05]");
    EXPECT_EQ(query.value().to_string("num"), "[03 TO 05]");

    auto two_dims = PointRangeQuery::create("num", {1, 4}, {6, 6}, 2);
    ASSERT_TRUE(two_dims.has_value());
    EXPECT_EQ(two_dims.value().to_string("num"), "[01 TO 06],[04 TO 06]");
}

TEST(PointRangeQueryTest, CreateValidatesTheEndpoints) {
    EXPECT_FALSE(PointRangeQuery::create("num", {}, {5}, 1).has_value());
    EXPECT_FALSE(PointRangeQuery::create("num", {1}, {5, 9}, 1).has_value());
    EXPECT_FALSE(PointRangeQuery::create("num", {1, 2}, {5, 9}, 3).has_value());
    EXPECT_FALSE(PointRangeQuery::create("num", {1}, {5}, 0).has_value());
}

TEST(PointRangeQueryTest, EqualityComparesTheBounds) {
    auto left = PointRangeQuery::create("num", {3}, {5}, 1);
    auto same = PointRangeQuery::create("num", {3}, {5}, 1);
    auto other = PointRangeQuery::create("num", {3}, {6}, 1);
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(same.has_value());
    ASSERT_TRUE(other.has_value());
    EXPECT_TRUE(left.value().equals(same.value()));
    EXPECT_EQ(left.value().hash(), same.value().hash());
    EXPECT_FALSE(left.value().equals(other.value()));
}

TEST(PointRangeQueryTest, ExplainReportsMatchesAndMisses) {
    std::map<int32_t, std::vector<uint8_t>> points{{0, {1}}, {1, {3}}, {2, {5}}};
    const IndexSearcher searcher = make_searcher(std::make_shared<MapPointValues>(std::move(points), 1));

    auto query = PointRangeQuery::create("num", {3}, {5}, 1);
    ASSERT_TRUE(query.has_value());
    auto weight = query.value().create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    auto matched = weight.value()->explain(leaves.value()[0], 1, searcher);
    ASSERT_TRUE(matched.has_value()) << matched.error().to_string();
    EXPECT_TRUE(matched.value().is_match());

    auto missed = weight.value()->explain(leaves.value()[0], 0, searcher);
    ASSERT_TRUE(missed.has_value()) << missed.error().to_string();
    EXPECT_FALSE(missed.value().is_match());
}

// ---------------------------------------------------------------------------
// PointInSetQuery
// ---------------------------------------------------------------------------

TEST(PointInSetQueryTest, MatchesThePointsOfTheSet) {
    std::map<int32_t, std::vector<uint8_t>> points{{0, {1}}, {1, {3}}, {2, {5}}, {3, {7}}};
    const IndexSearcher searcher = make_searcher(std::make_shared<MapPointValues>(std::move(points), 1));

    auto query = PointInSetQuery::create("num", {{3}, {7}}, 1);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{1, 3}));
}

TEST(PointInSetQueryTest, TheSetIsSortedAndDeduplicated) {
    auto query = PointInSetQuery::create("num", {{7}, {3}, {7}}, 1);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    ASSERT_EQ(query.value().points().size(), 2);
    EXPECT_EQ(query.value().points()[0], (std::vector<uint8_t>{3}));
    EXPECT_EQ(query.value().points()[1], (std::vector<uint8_t>{7}));
}

TEST(PointInSetQueryTest, TwoDimensionsHaveToMatchExactly) {
    std::map<int32_t, std::vector<uint8_t>> points{{0, {1, 1}}, {1, {1, 5}}, {2, {5, 5}}};
    const IndexSearcher searcher = make_searcher(std::make_shared<MapPointValues>(std::move(points), 2));

    auto query = PointInSetQuery::create("num", {{5, 5}}, 2);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{2}));
}

TEST(PointInSetQueryTest, ASetWithoutAnyMatchMatchesNothing) {
    std::map<int32_t, std::vector<uint8_t>> points{{0, {1}}, {1, {3}}};
    const IndexSearcher searcher = make_searcher(std::make_shared<MapPointValues>(std::move(points), 1));

    auto query = PointInSetQuery::create("num", {{9}}, 1);
    ASSERT_TRUE(query.has_value());
    EXPECT_TRUE(matching_docs(query.value(), searcher).empty());
}

TEST(PointInSetQueryTest, MismatchedDimensionsAreRejected) {
    std::map<int32_t, std::vector<uint8_t>> points{{0, {1}}};
    const IndexSearcher searcher = make_searcher(std::make_shared<MapPointValues>(std::move(points), 1));

    auto query = PointInSetQuery::create("num", {{0, 0}}, 2);
    ASSERT_TRUE(query.has_value());
    auto weight = query.value().create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());
    auto supplier = weight.value()->scorer_supplier(leaves.value()[0], searcher);
    ASSERT_FALSE(supplier.has_value());
    EXPECT_EQ(supplier.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(PointInSetQueryTest, ToStringPrintsTheSet) {
    auto query = PointInSetQuery::create("num", {{3}, {7}}, 1);
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(query.value().to_string(""), "num:{03,07}");
    EXPECT_EQ(query.value().to_string("num"), "{03,07}");
}

TEST(PointInSetQueryTest, CreateValidatesThePoints) {
    EXPECT_FALSE(PointInSetQuery::create("num", {}, 1).has_value());
    EXPECT_FALSE(PointInSetQuery::create("num", {{}}, 1).has_value());
    EXPECT_FALSE(PointInSetQuery::create("num", {{1}, {2, 3}}, 1).has_value());
    EXPECT_FALSE(PointInSetQuery::create("num", {{1, 2}}, 3).has_value());
    EXPECT_FALSE(PointInSetQuery::create("num", {{1}}, 0).has_value());
}

TEST(PointInSetQueryTest, EqualityComparesThePoints) {
    auto left = PointInSetQuery::create("num", {{3}, {7}}, 1);
    auto same = PointInSetQuery::create("num", {{7}, {3}}, 1);
    auto other = PointInSetQuery::create("num", {{3}}, 1);
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(same.has_value());
    ASSERT_TRUE(other.has_value());
    EXPECT_TRUE(left.value().equals(same.value()));
    EXPECT_EQ(left.value().hash(), same.value().hash());
    EXPECT_FALSE(left.value().equals(other.value()));
}

TEST(PointInSetQueryTest, ExplainReportsMatchesAndMisses) {
    std::map<int32_t, std::vector<uint8_t>> points{{0, {1}}, {1, {3}}, {2, {5}}};
    const IndexSearcher searcher = make_searcher(std::make_shared<MapPointValues>(std::move(points), 1));

    auto query = PointInSetQuery::create("num", {{3}, {5}}, 1);
    ASSERT_TRUE(query.has_value());
    auto weight = query.value().create_weight(searcher, ScoreMode::Complete, 2.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    auto matched = weight.value()->explain(leaves.value()[0], 2, searcher);
    ASSERT_TRUE(matched.has_value()) << matched.error().to_string();
    EXPECT_TRUE(matched.value().is_match());
    EXPECT_FLOAT_EQ(static_cast<float>(number_as_f64(matched.value().value())), 2.0F);

    auto missed = weight.value()->explain(leaves.value()[0], 0, searcher);
    ASSERT_TRUE(missed.has_value()) << missed.error().to_string();
    EXPECT_FALSE(missed.value().is_match());
}
