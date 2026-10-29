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
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.sandbox_fields_and_collector;

import lucene_core;

using namespace lucene;

namespace {

std::vector<std::vector<uint8_t>> encode_all(std::span<const float> values) {
    std::vector<std::vector<uint8_t>> encoded;
    for (const float value : values) {
        auto packed = HalfFloatPoint::pack(std::span<const float>(&value, 1));
        EXPECT_TRUE(packed.has_value());
        encoded.push_back(packed.value_or(std::vector<uint8_t>{}));
    }
    return encoded;
}

std::shared_ptr<DirectoryReader> index_points(std::span<const float> values) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    EXPECT_TRUE(writer.has_value());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (const float value : values) {
        auto field = HalfFloatPoint::create("p", std::span<const float>(&value, 1));
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

TEST(HalfFloatPointTest, EncodesAndDecodesValues) {
    const std::vector<float> values{0.0F, 0.5F, 1.0F, 1.5F, -1.0F, -2.5F, 65504.0F};
    for (const float value : values) {
        std::vector<uint8_t> buffer(HalfFloatPoint::BYTES, 0);
        HalfFloatPoint::encode_dimension(value, buffer, 0);
        EXPECT_EQ(HalfFloatPoint::decode_dimension(buffer, 0), value) << value;
    }

    // The sortable form of 1.0 is 0x3C00, biased by the sign flip.
    std::vector<uint8_t> one(HalfFloatPoint::BYTES, 0);
    HalfFloatPoint::encode_dimension(1.0F, one, 0);
    EXPECT_EQ(one, (std::vector<uint8_t>{0xBC, 0x00}));
}

TEST(HalfFloatPointTest, ByteOrderEqualsNumericOrder) {
    const std::vector<float> ascending{-100.0F, -1.5F, -0.0F, 0.0F, 0.25F, 1.0F, 42.0F, 1000.0F};
    const auto encoded = encode_all(ascending);
    for (size_t i = 1; i < encoded.size(); ++i) {
        EXPECT_LT(encoded[i - 1], encoded[i]) << "index " << i;
    }
}

TEST(HalfFloatPointTest, RoundsToTheNearestHalfFloat) {
    // 0.1 has no exact half-float representation; it rounds to 0.0999755859375.
    std::vector<uint8_t> buffer(HalfFloatPoint::BYTES, 0);
    HalfFloatPoint::encode_dimension(0.1F, buffer, 0);
    const float decoded = HalfFloatPoint::decode_dimension(buffer, 0);
    EXPECT_FLOAT_EQ(decoded, 0.0999755859375F);
    EXPECT_LT(std::abs(decoded - 0.1F), 0.0001F);

    // Ties round to even: 2049 is exactly between 2048 and 2050.
    HalfFloatPoint::encode_dimension(2049.0F, buffer, 0);
    EXPECT_EQ(HalfFloatPoint::decode_dimension(buffer, 0), 2048.0F);
}

TEST(HalfFloatPointTest, ComputesNextUpAndDown) {
    EXPECT_GT(HalfFloatPoint::next_up(1.0F), 1.0F);
    EXPECT_LT(HalfFloatPoint::next_down(1.0F), 1.0F);
    // There is no half float between 1.0 and next_up(1.0).
    EXPECT_EQ(HalfFloatPoint::next_down(HalfFloatPoint::next_up(1.0F)), 1.0F);
    EXPECT_EQ(HalfFloatPoint::next_up(HalfFloatPoint::next_down(1.0F)), 1.0F);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(std::isnan(HalfFloatPoint::next_up(nan)));
    EXPECT_TRUE(std::isnan(HalfFloatPoint::next_down(nan)));
    EXPECT_EQ(HalfFloatPoint::next_up(std::numeric_limits<float>::infinity()),
              std::numeric_limits<float>::infinity());
    EXPECT_EQ(HalfFloatPoint::next_down(-std::numeric_limits<float>::infinity()),
              -std::numeric_limits<float>::infinity());
}

TEST(HalfFloatPointTest, CreatesFieldsAndRejectsInvalidPoints) {
    const std::vector<float> point{1.0F, 2.0F};
    auto field = HalfFloatPoint::create("p", point);
    ASSERT_TRUE(field.has_value()) << (field.has_value() ? "" : field.error().to_string());
    EXPECT_EQ(field.value().field_type().point_dimension_count(), 2U);
    EXPECT_EQ(field.value().field_type().point_num_bytes(), 2U);
    EXPECT_EQ(field.value().binary_value().value().size(), 4U);

    const std::vector<float> empty;
    EXPECT_FALSE(HalfFloatPoint::create("p", empty).has_value());
    const std::vector<float> too_many(9, 1.0F);
    EXPECT_FALSE(HalfFloatPoint::create("p", too_many).has_value());
    const std::vector<float> empty_set;
    EXPECT_FALSE(HalfFloatPoint::new_set_query("p", empty_set).has_value());
}

TEST(HalfFloatPointTest, AnswersRangeAndSetQueries) {
    const std::vector<float> points{1.0F, 2.0F, 3.0F, 4.0F};
    auto reader = index_points(points);
    ASSERT_NE(reader, nullptr);

    auto exact = HalfFloatPoint::new_exact_query("p", 2.0F);
    ASSERT_TRUE(exact.has_value()) << (exact.has_value() ? "" : exact.error().to_string());
    EXPECT_EQ(search_docs(*exact.value(), reader), (std::vector<int32_t>{1}));

    auto range = HalfFloatPoint::new_range_query("p", 2.0F, 3.0F);
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(search_docs(*range.value(), reader), (std::vector<int32_t>{1, 2}));

    // Half-open interval via next_up.
    auto exclusive = HalfFloatPoint::new_range_query("p", HalfFloatPoint::next_up(2.0F), 4.0F);
    ASSERT_TRUE(exclusive.has_value());
    EXPECT_EQ(search_docs(*exclusive.value(), reader), (std::vector<int32_t>{2, 3}));

    const std::vector<float> set_values{1.0F, 4.0F};
    auto set = HalfFloatPoint::new_set_query("p", set_values);
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(search_docs(*set.value(), reader), (std::vector<int32_t>{0, 3}));
}

TEST(LargeNumHitsTopDocsCollectorTest, KeepsTheBestHitsOfMany) {
    const std::vector<float> points{1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
    auto reader = index_points(points);
    ASSERT_NE(reader, nullptr);

    // Every document matches a 1.0 query boost-wise, so use a match-all-ish range.
    auto query = HalfFloatPoint::new_range_query("p", 0.0F, 10.0F);
    ASSERT_TRUE(query.has_value());

    IndexSearcher searcher(reader);
    LargeNumHitsTopDocsCollector collector(3);
    ASSERT_TRUE(searcher.search_with_collector(*query.value(), collector).has_value());
    EXPECT_EQ(collector.total_hits(), 5U);

    auto top = collector.top_docs();
    ASSERT_TRUE(top.has_value()) << (top.has_value() ? "" : top.error().to_string());
    EXPECT_EQ(top.value().total_hits.value, 5U);
    EXPECT_EQ(top.value().total_hits.relation, TotalHits::Relation::EqualTo);
    EXPECT_EQ(top.value().score_docs.size(), 3U);

    // The collector reports the smallest document numbers when the scores tie.
    std::vector<int32_t> docs;
    for (const ScoreDoc &hit : top.value().score_docs) {
        docs.push_back(hit.doc);
    }
    std::ranges::sort(docs);
    EXPECT_EQ(docs, (std::vector<int32_t>{0, 1, 2}));
}

TEST(LargeNumHitsTopDocsCollectorTest, ReturnsEverythingBelowTheLimit) {
    auto reader = index_points(std::vector<float>{1.0F, 2.0F});
    ASSERT_NE(reader, nullptr);

    auto query = HalfFloatPoint::new_range_query("p", 0.0F, 10.0F);
    ASSERT_TRUE(query.has_value());

    IndexSearcher searcher(reader);
    LargeNumHitsTopDocsCollector collector(10);
    ASSERT_TRUE(searcher.search_with_collector(*query.value(), collector).has_value());
    EXPECT_EQ(collector.total_hits(), 2U);

    auto top = collector.top_docs();
    ASSERT_TRUE(top.has_value());
    // Fewer hits than requested: everything comes back.
    EXPECT_EQ(top.value().score_docs.size(), 2U);

    // Asking for more than was collected is an error.
    EXPECT_FALSE(collector.top_docs_with_how_many(3).has_value());
    // So is asking for nothing.
    EXPECT_FALSE(collector.top_docs_with_how_many(0).has_value());
    // Asking for a prefix of the hits works.
    auto one = collector.top_docs_with_how_many(1);
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one.value().score_docs.size(), 1U);
}
