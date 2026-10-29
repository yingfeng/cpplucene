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
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.big_integer_point;

import lucene_core;

using namespace lucene;

namespace {

Int128 from_decimal(std::string_view text) {
    auto parsed = BigIntegerPoint::parse(text);
    EXPECT_TRUE(parsed.has_value()) << (parsed.has_value() ? "" : parsed.error().to_string());
    return parsed.value_or(0);
}

std::shared_ptr<DirectoryReader> index_points(std::span<const Int128> values) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    EXPECT_TRUE(writer.has_value());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (const Int128 value : values) {
        auto field = BigIntegerPoint::create("p", std::span<const Int128>(&value, 1));
        EXPECT_TRUE(field.has_value()) << (field.has_value() ? "" : field.error().to_string());
        Document doc;
        doc.add(std::make_unique<Field>(std::move(field.value())));
        EXPECT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    auto reader = DirectoryReader::open(*dir);
    EXPECT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

std::vector<int32_t> search_docs(const Query &query, const std::shared_ptr<DirectoryReader> &reader) {
    IndexSearcher searcher(reader);
    auto top = searcher.search(query, 10);
    EXPECT_TRUE(top.has_value()) << (top.has_value() ? "" : top.error().to_string());
    std::vector<int32_t> docs;
    if (!top.has_value()) {
        return docs;
    }
    for (const ScoreDoc &hit : top.value().score_docs) {
        docs.push_back(hit.doc);
    }
    std::ranges::sort(docs);
    return docs;
}

} // namespace

TEST(BigIntegerPointTest, ParsesAndFormatsDecimals) {
    EXPECT_EQ(BigIntegerPoint::to_string(from_decimal("0")), "0");
    EXPECT_EQ(BigIntegerPoint::to_string(from_decimal("-1")), "-1");
    EXPECT_EQ(BigIntegerPoint::to_string(from_decimal("123456789012345678901234567890")),
              "123456789012345678901234567890");
    EXPECT_EQ(BigIntegerPoint::to_string(from_decimal("-123456789012345678901234567890")),
              "-123456789012345678901234567890");
    EXPECT_EQ(from_decimal("+42"), 42);

    // The 128-bit limits are exact.
    EXPECT_EQ(BigIntegerPoint::to_string(BigIntegerPoint::min_value()), "-170141183460469231731687303715884105728");
    EXPECT_EQ(BigIntegerPoint::to_string(BigIntegerPoint::max_value()), "170141183460469231731687303715884105727");

    auto too_big = BigIntegerPoint::parse("170141183460469231731687303715884105728");
    ASSERT_FALSE(too_big.has_value());
    EXPECT_EQ(too_big.error().kind(), ErrorKind::kNumberOverflow);
    auto too_small = BigIntegerPoint::parse("-170141183460469231731687303715884105729");
    ASSERT_FALSE(too_small.has_value());
    EXPECT_EQ(too_small.error().kind(), ErrorKind::kNumberOverflow);
    auto garbage = BigIntegerPoint::parse("12x");
    ASSERT_FALSE(garbage.has_value());
    EXPECT_EQ(garbage.error().kind(), ErrorKind::kNumberFormat);
    EXPECT_FALSE(BigIntegerPoint::parse("").has_value());
}

TEST(BigIntegerPointTest, ByteOrderEqualsNumericOrder) {
    const std::vector<Int128> ascending{BigIntegerPoint::min_value(), -1000, -1, 0, 1, 1000,
                                        BigIntegerPoint::max_value()};
    std::vector<std::vector<uint8_t>> encoded;
    for (const Int128 value : ascending) {
        const std::array<Int128, 1> single{value};
        auto packed = BigIntegerPoint::pack(single);
        ASSERT_TRUE(packed.has_value());
        encoded.push_back(packed.value());
        EXPECT_EQ(BigIntegerPoint::decode_dimension(packed.value(), 0), value);
    }
    for (size_t i = 1; i < encoded.size(); ++i) {
        EXPECT_LT(encoded[i - 1], encoded[i]) << "index " << i;
    }
}

TEST(BigIntegerPointTest, CreatesFieldsAndRejectsInvalidPoints) {
    const std::vector<Int128> point{1, 2};
    auto field = BigIntegerPoint::create("p", point);
    ASSERT_TRUE(field.has_value()) << (field.has_value() ? "" : field.error().to_string());
    EXPECT_EQ(field.value().field_type().point_dimension_count(), 2U);
    EXPECT_EQ(field.value().field_type().point_num_bytes(), 16U);
    EXPECT_EQ(field.value().binary_value().value().size(), 32U);

    const std::vector<Int128> empty;
    EXPECT_FALSE(BigIntegerPoint::create("p", empty).has_value());
    const std::vector<Int128> too_many(9, 1);
    EXPECT_FALSE(BigIntegerPoint::create("p", too_many).has_value());
    const std::vector<Int128> empty_set;
    EXPECT_FALSE(BigIntegerPoint::new_set_query("p", empty_set).has_value());
}

TEST(BigIntegerPointTest, AnswersRangeAndSetQueries) {
    const std::vector<Int128> points{from_decimal("-5000000000000000000000"), 5,
                                     from_decimal("5000000000000000000000"),
                                     from_decimal("9000000000000000000000")};
    auto reader = index_points(points);
    ASSERT_NE(reader, nullptr);

    auto exact = BigIntegerPoint::new_exact_query("p", 5);
    ASSERT_TRUE(exact.has_value()) << (exact.has_value() ? "" : exact.error().to_string());
    EXPECT_EQ(search_docs(*exact.value(), reader), (std::vector<int32_t>{1}));

    // A range spanning the negative and positive big values.
    auto range = BigIntegerPoint::new_range_query("p", from_decimal("-5000000000000000000000"), 5);
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(search_docs(*range.value(), reader), (std::vector<int32_t>{0, 1}));

    const std::vector<Int128> set_values{points[0], points[3]};
    auto set = BigIntegerPoint::new_set_query("p", set_values);
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(search_docs(*set.value(), reader), (std::vector<int32_t>{0, 3}));

    // The full 128-bit domain matches everything.
    auto everything =
        BigIntegerPoint::new_range_query("p", BigIntegerPoint::min_value(), BigIntegerPoint::max_value());
    ASSERT_TRUE(everything.has_value());
    EXPECT_EQ(search_docs(*everything.value(), reader), (std::vector<int32_t>{0, 1, 2, 3}));
}
