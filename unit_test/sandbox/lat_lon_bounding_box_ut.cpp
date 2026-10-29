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

module lucene_core:ut.lat_lon_bounding_box;

import lucene_core;

using namespace lucene;

namespace {

/// `[minLat, minLon] .. [maxLat, maxLon]` per document, indexed by `doc`.
struct Box {
    double min_lat;
    double min_lon;
    double max_lat;
    double max_lon;
};

std::shared_ptr<DirectoryReader> index_boxes(std::span<const Box> boxes) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    EXPECT_TRUE(writer.has_value());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (const Box &box : boxes) {
        auto field =
            LatLonBoundingBox::create("b", box.min_lat, box.min_lon, box.max_lat, box.max_lon);
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

TEST(LatLonBoundingBoxTest, EncodesFourCoordinatesAndRejectsBadBoxes) {
    auto encoded = LatLonBoundingBox::encode(-90.0, -180.0, 90.0, 180.0);
    ASSERT_TRUE(encoded.has_value()) << (encoded.has_value() ? "" : encoded.error().to_string());
    EXPECT_EQ(encoded.value().size(), 16U);

    auto field = LatLonBoundingBox::create("b", 10.0, 20.0, 30.0, 40.0);
    ASSERT_TRUE(field.has_value()) << (field.has_value() ? "" : field.error().to_string());
    EXPECT_EQ(field.value().field_type().point_dimension_count(), 4U);
    EXPECT_EQ(field.value().field_type().point_num_bytes(), 4U);
    EXPECT_EQ(field.value().binary_value().value().size(), 16U);

    // minLon > maxLon and minLat > maxLat are rejected.
    EXPECT_FALSE(LatLonBoundingBox::create("b", 0.0, 10.0, 10.0, 5.0).has_value());
    EXPECT_FALSE(LatLonBoundingBox::create("b", 10.0, 0.0, 5.0, 10.0).has_value());

    const std::string text = LatLonBoundingBox::to_string(encoded.value());
    EXPECT_NE(text.find("LatLonBoundingBox"), std::string::npos);
}

TEST(LatLonBoundingBoxTest, AnswersRelationQueries) {
    // 0: a box around Paris, 1: a box around western Europe, 2: an unrelated box.
    const std::vector<Box> boxes{{48.8, 2.2, 48.9, 2.4}, {40.0, -5.0, 55.0, 15.0}, {-10.0, -10.0, -5.0, -5.0}};
    auto reader = index_boxes(boxes);
    ASSERT_NE(reader, nullptr);

    // Paris intersects the bigger Europe box.
    auto intersects = LatLonBoundingBox::new_intersects_query("b", 41.0, -4.0, 54.0, 14.0);
    ASSERT_TRUE(intersects.has_value()) << (intersects.has_value() ? "" : intersects.error().to_string());
    EXPECT_EQ(search_docs(*intersects.value(), reader), (std::vector<int32_t>{0, 1}));

    // Only the big box contains the small query box.
    auto contains = LatLonBoundingBox::new_contains_query("b", 45.0, 0.0, 50.0, 5.0);
    ASSERT_TRUE(contains.has_value());
    EXPECT_EQ(search_docs(*contains.value(), reader), (std::vector<int32_t>{1}));

    // Only the Paris box lies inside the small query box.
    auto within = LatLonBoundingBox::new_within_query("b", 40.0, -5.0, 55.0, 15.0);
    ASSERT_TRUE(within.has_value());
    EXPECT_EQ(search_docs(*within.value(), reader), (std::vector<int32_t>{0, 1}));

    // A box that crosses the boundary of the query box: neither within nor
    // containing it.
    auto crosses = LatLonBoundingBox::new_crosses_query("b", 45.0, 0.0, 50.0, 5.0);
    ASSERT_TRUE(crosses.has_value());
    EXPECT_EQ(search_docs(*crosses.value(), reader), (std::vector<int32_t>{1}));

    // A query box far away matches nothing.
    auto far = LatLonBoundingBox::new_intersects_query("b", 0.0, 0.0, 1.0, 1.0);
    ASSERT_TRUE(far.has_value());
    EXPECT_TRUE(search_docs(*far.value(), reader).empty());
}
