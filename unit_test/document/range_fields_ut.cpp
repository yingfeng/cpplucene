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

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.range_fields;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kField = "range";

std::vector<int32_t> ints(std::initializer_list<int32_t> values) {
    return std::vector<int32_t>(values);
}

std::vector<int64_t> longs(std::initializer_list<int64_t> values) {
    return std::vector<int64_t>(values);
}

std::vector<float> floats(std::initializer_list<float> values) {
    return std::vector<float>(values);
}

std::vector<double> doubles(std::initializer_list<double> values) {
    return std::vector<double>(values);
}

std::vector<int32_t> search_docs(const Query &query, std::shared_ptr<DirectoryReader> &reader) {
    std::vector<int32_t> docs;
    IndexSearcher searcher(reader);
    auto top = searcher.search(query, 10);
    EXPECT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());
    if (!top.has_value()) {
        return docs;
    }
    for (const ScoreDoc &hit : top.value().score_docs) {
        docs.push_back(hit.doc);
    }
    std::ranges::sort(docs);
    return docs;
}

/// Indexes one `IntRange` per document and reopens the reader.
std::shared_ptr<DirectoryReader> index_int_ranges(const std::vector<std::pair<int32_t, int32_t>> &ranges) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    EXPECT_TRUE(writer.has_value());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (const auto &[min, max] : ranges) {
        auto range = IntRange::create(std::string(kField), ints({min}), ints({max}));
        EXPECT_TRUE(range.has_value()) << (range ? "" : range.error().to_string());
        Document doc;
        doc.add(std::make_unique<IntRange>(std::move(range.value())));
        EXPECT_TRUE(writer.value()->add_document(doc).has_value());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    auto reader = DirectoryReader::open(*dir);
    EXPECT_TRUE(reader.has_value());
    return reader.has_value() ? reader.value() : nullptr;
}

} // namespace

TEST(RangeFieldsTest, EncodesAndDecodesIntRanges) {
    auto range = IntRange::create(std::string(kField), ints({10, 0}), ints({20, 5}));
    ASSERT_TRUE(range.has_value()) << (range ? "" : range.error().to_string());
    const IntRange &field = range.value();

    EXPECT_EQ(field.name(), kField);
    EXPECT_EQ(field.field_type().point_dimension_count(), 4U);
    EXPECT_EQ(field.field_type().point_num_bytes(), 4U);
    auto bytes = field.binary_value();
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes.value().size(), 16U);

    EXPECT_EQ(field.get_min(0).value(), 10);
    EXPECT_EQ(field.get_max(0).value(), 20);
    EXPECT_EQ(field.get_min(1).value(), 0);
    EXPECT_EQ(field.get_max(1).value(), 5);
    EXPECT_FALSE(field.get_min(2).has_value());
    EXPECT_FALSE(field.numeric_value().has_value());
    EXPECT_EQ(field.to_string(), "IntRange <range: [10 : 20] [0 : 5]>");
}

TEST(RangeFieldsTest, ValidatesIntRangeBounds) {
    auto empty = IntRange::create(std::string(kField), std::vector<int32_t>{}, std::vector<int32_t>{});
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().kind(), ErrorKind::kIllegalArgument);

    auto mismatch = IntRange::create(std::string(kField), ints({1, 2}), ints({3}));
    ASSERT_FALSE(mismatch.has_value());

    auto too_many = IntRange::create(std::string(kField), ints({1, 1, 1, 1, 1}), ints({2, 2, 2, 2, 2}));
    ASSERT_FALSE(too_many.has_value());

    auto inverted = IntRange::create(std::string(kField), ints({5}), ints({1}));
    ASSERT_FALSE(inverted.has_value());
    EXPECT_EQ(inverted.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(RangeFieldsTest, ReplacesTheValuesWithoutChangingTheDimensions) {
    auto range = IntRange::create(std::string(kField), ints({1, 2}), ints({3, 4}));
    ASSERT_TRUE(range.has_value());
    ASSERT_TRUE(range.value().set_range_values(ints({7, 8}), ints({9, 10})).has_value());
    EXPECT_EQ(range.value().get_min(0).value(), 7);
    EXPECT_EQ(range.value().get_max(1).value(), 10);

    auto wrong = range.value().set_range_values(ints({1}), ints({2}));
    ASSERT_FALSE(wrong.has_value());
    EXPECT_EQ(wrong.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(RangeFieldsTest, EncodesTheOtherNumberTypes) {
    auto long_range = LongRange::create(std::string(kField), longs({std::numeric_limits<int64_t>::min()}),
                                        longs({std::numeric_limits<int64_t>::max()}));
    ASSERT_TRUE(long_range.has_value()) << (long_range ? "" : long_range.error().to_string());
    EXPECT_EQ(long_range.value().get_min(0).value(), std::numeric_limits<int64_t>::min());
    EXPECT_EQ(long_range.value().get_max(0).value(), std::numeric_limits<int64_t>::max());
    EXPECT_EQ(long_range.value().field_type().point_num_bytes(), 8U);

    auto float_range = FloatRange::create(std::string(kField), floats({-1.5F}), floats({2.25F}));
    ASSERT_TRUE(float_range.has_value()) << (float_range ? "" : float_range.error().to_string());
    EXPECT_FLOAT_EQ(float_range.value().get_min(0).value(), -1.5F);
    EXPECT_FLOAT_EQ(float_range.value().get_max(0).value(), 2.25F);
    EXPECT_EQ(float_range.value().to_string(), "FloatRange <range: [-1.5 : 2.25]>");

    auto double_range = DoubleRange::create(std::string(kField), doubles({-1.0E10}), doubles({1.0E10}));
    ASSERT_TRUE(double_range.has_value()) << (double_range ? "" : double_range.error().to_string());
    EXPECT_DOUBLE_EQ(double_range.value().get_min(0).value(), -1.0E10);
    EXPECT_DOUBLE_EQ(double_range.value().get_max(0).value(), 1.0E10);
    EXPECT_EQ(double_range.value().field_type().point_dimension_count(), 2U);

    auto nan = FloatRange::create(std::string(kField), floats({std::numeric_limits<float>::quiet_NaN()}),
                                  floats({1.0F}));
    ASSERT_FALSE(nan.has_value());
    EXPECT_EQ(nan.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(RangeFieldsTest, QueriesTheRelationOfTheIndexedRanges) {
    // Documents: [10,20], [15,25], [30,40].
    auto reader = index_int_ranges({{10, 20}, {15, 25}, {30, 40}});
    ASSERT_NE(reader, nullptr);

    auto intersects = IntRange::new_intersects_query(std::string(kField), ints({18}), ints({22}));
    ASSERT_TRUE(intersects.has_value()) << (intersects ? "" : intersects.error().to_string());
    EXPECT_EQ(search_docs(intersects.value(), reader), (std::vector<int32_t>{0, 1}));

    auto within = IntRange::new_within_query(std::string(kField), ints({0}), ints({100}));
    ASSERT_TRUE(within.has_value());
    EXPECT_EQ(search_docs(within.value(), reader), (std::vector<int32_t>{0, 1, 2}));

    auto contains = IntRange::new_contains_query(std::string(kField), ints({18}), ints({22}));
    ASSERT_TRUE(contains.has_value());
    EXPECT_EQ(search_docs(contains.value(), reader), (std::vector<int32_t>{1}));

    auto crosses = IntRange::new_crosses_query(std::string(kField), ints({18}), ints({22}));
    ASSERT_TRUE(crosses.has_value());
    EXPECT_EQ(search_docs(crosses.value(), reader), (std::vector<int32_t>{0, 1}));

    auto disjoint = IntRange::new_intersects_query(std::string(kField), ints({100}), ints({200}));
    ASSERT_TRUE(disjoint.has_value());
    EXPECT_TRUE(search_docs(disjoint.value(), reader).empty());
}

TEST(RangeFieldsTest, QueriesOpenEndedRanges) {
    // A document with an open ended range, like a zip code range.
    auto reader = index_int_ranges({{std::numeric_limits<int32_t>::min(), 5},
                                    {6, std::numeric_limits<int32_t>::max()}});
    ASSERT_NE(reader, nullptr);

    auto low = IntRange::new_intersects_query(std::string(kField), ints({std::numeric_limits<int32_t>::min()}),
                                              ints({5}));
    ASSERT_TRUE(low.has_value());
    // The second document is unbounded on the right, so it only intersects the
    // first range as a cell, not as a value.
    EXPECT_EQ(search_docs(low.value(), reader), (std::vector<int32_t>{0}));

    auto everything = IntRange::new_intersects_query(std::string(kField),
                                                     ints({std::numeric_limits<int32_t>::min()}),
                                                     ints({std::numeric_limits<int32_t>::max()}));
    ASSERT_TRUE(everything.has_value());
    EXPECT_EQ(search_docs(everything.value(), reader), (std::vector<int32_t>{0, 1}));
}

TEST(RangeFieldsTest, QueriesMultiDimensionalRanges) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());

    const std::vector<std::pair<std::pair<int32_t, int32_t>, std::pair<int32_t, int32_t>>> boxes{
            {{0, 10}, {0, 10}},
            {{5, 15}, {5, 15}},
            {{100, 110}, {100, 110}},
    };
    for (const auto &box : boxes) {
        auto range = IntRange::create(std::string(kField), ints({box.first.first, box.second.first}),
                                      ints({box.first.second, box.second.second}));
        ASSERT_TRUE(range.has_value());
        Document doc;
        doc.add(std::make_unique<IntRange>(std::move(range.value())));
        ASSERT_TRUE(writer.value()->add_document(doc).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    auto reader = DirectoryReader::open(*dir);
    ASSERT_TRUE(reader.has_value());

    auto query = IntRange::new_contains_query(std::string(kField), ints({6, 6}), ints({9, 9}));
    ASSERT_TRUE(query.has_value());
    auto text = query.value().to_string(std::string(kField));
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value(), "<ranges:[6 : 9] [6 : 9]>");
    auto qualified = query.value().to_string("other");
    ASSERT_TRUE(qualified.has_value());
    EXPECT_EQ(qualified.value(), "range:<ranges:[6 : 9] [6 : 9]>");
    EXPECT_EQ(search_docs(query.value(), reader.value()), (std::vector<int32_t>{0, 1}));
}

TEST(RangeFieldsTest, QueriesFloatAndDoubleRanges) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());

    auto float_range = FloatRange::create("score", floats({1.0F}), floats({2.0F}));
    ASSERT_TRUE(float_range.has_value());
    Document first;
    first.add(std::make_unique<FloatRange>(std::move(float_range.value())));
    ASSERT_TRUE(writer.value()->add_document(first).has_value());

    auto double_range = DoubleRange::create("weight", doubles({10.0}), doubles({20.0}));
    ASSERT_TRUE(double_range.has_value());
    Document second;
    second.add(std::make_unique<DoubleRange>(std::move(double_range.value())));
    ASSERT_TRUE(writer.value()->add_document(second).has_value());

    auto long_range = LongRange::create("span", longs({5}), longs({9}));
    ASSERT_TRUE(long_range.has_value());
    Document third;
    third.add(std::make_unique<LongRange>(std::move(long_range.value())));
    ASSERT_TRUE(writer.value()->add_document(third).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(*dir);
    ASSERT_TRUE(reader.has_value());

    auto float_query = FloatRange::new_intersects_query("score", floats({1.5F}), floats({1.5F}));
    ASSERT_TRUE(float_query.has_value());
    EXPECT_EQ(search_docs(float_query.value(), reader.value()), (std::vector<int32_t>{0}));

    auto double_query = DoubleRange::new_intersects_query("weight", doubles({15.0}), doubles({15.0}));
    ASSERT_TRUE(double_query.has_value());
    EXPECT_EQ(search_docs(double_query.value(), reader.value()), (std::vector<int32_t>{1}));

    auto long_query = LongRange::new_within_query("span", longs({0}), longs({100}));
    ASSERT_TRUE(long_query.has_value());
    EXPECT_EQ(search_docs(long_query.value(), reader.value()), (std::vector<int32_t>{2}));
}

TEST(RangeFieldsTest, RejectsInvalidQueryArguments) {
    auto empty = IntRange::new_intersects_query(std::string(kField), std::vector<int32_t>{}, std::vector<int32_t>{});
    ASSERT_FALSE(empty.has_value());

    auto inverted = LongRange::new_contains_query(std::string(kField), longs({10}), longs({1}));
    ASSERT_FALSE(inverted.has_value());
    EXPECT_EQ(inverted.error().kind(), ErrorKind::kIllegalArgument);

    auto nan = DoubleRange::new_within_query(std::string(kField),
                                             doubles({std::numeric_limits<double>::quiet_NaN()}), doubles({1.0}));
    ASSERT_FALSE(nan.has_value());
}
