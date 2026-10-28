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

// `LatLonPoint::new_box_query`: a box on the map, including one that crosses the
// antimeridian, has to match exactly the points inside it.

module;

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.lat_lon_point_box_query;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kField = "location";

struct Location {
    double latitude{0.0};
    double longitude{0.0};
};

/// The locations the index holds: a grid that reaches across the antimeridian.
const std::vector<Location> &locations() {
    static const std::vector<Location> points = [] {
        std::vector<Location> out;
        for (const double latitude : {-60.0, -10.0, 0.0, 10.0, 60.0}) {
            for (const double longitude : {-170.0, -100.0, -10.0, 0.0, 10.0, 100.0, 170.0, 179.0, -179.0}) {
                out.push_back(Location{latitude, longitude});
            }
        }
        return out;
    }();
    return points;
}

void build(Directory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    for (const Location &location : locations()) {
        Document document;
        auto point = LatLonPoint::create(std::string(kField), location.latitude, location.longitude);
        ASSERT_TRUE(point.has_value()) << point.error().to_string();
        document.add(std::make_unique<Field>(std::move(point.value())));
        ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// The documents a box matches, from the source data.  A box whose
/// `max_longitude` is below its `min_longitude` crosses the antimeridian.
std::vector<int32_t> expected(double min_latitude, double max_latitude, double min_longitude, double max_longitude) {
    std::vector<int32_t> docs;
    for (int32_t doc = 0; doc < static_cast<int32_t>(locations().size()); ++doc) {
        const Location &location = locations()[static_cast<size_t>(doc)];
        if (location.latitude < min_latitude || location.latitude > max_latitude) {
            continue;
        }
        const bool inside_longitude = max_longitude < min_longitude
                                              ? (location.longitude >= min_longitude || location.longitude <= max_longitude)
                                              : (location.longitude >= min_longitude && location.longitude <= max_longitude);
        if (inside_longitude) {
            docs.push_back(doc);
        }
    }
    return docs;
}

std::vector<int32_t> hits_of(IndexSearcher &searcher, const std::shared_ptr<Query> &query) {
    auto hits = searcher.search(*query, static_cast<int32_t>(locations().size()));
    EXPECT_TRUE(hits.has_value()) << (hits.has_value() ? "" : hits.error().to_string());
    std::vector<int32_t> docs;
    if (!hits.has_value()) {
        return docs;
    }
    for (const ScoreDoc &hit : hits.value().score_docs) {
        docs.push_back(hit.doc);
    }
    std::sort(docs.begin(), docs.end());
    return docs;
}

} // namespace

TEST(LatLonPointBoxQueryTest, ABoxMatchesExactlyThePointsInsideIt) {
    ByteBuffersDirectory directory;
    build(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    struct Box {
        double min_latitude;
        double max_latitude;
        double min_longitude;
        double max_longitude;
    };
    const std::vector<Box> boxes{
            {-20.0, 20.0, -20.0, 20.0},  // around the origin
            {0.0, 60.0, -180.0, 180.0},  // the whole northern half
            {-90.0, 90.0, 99.0, 171.0},  // a longitude band
            {5.0, 5.0, -5.0, 5.0},       // a box without any point in it
    };
    for (const Box &box : boxes) {
        auto query = LatLonPoint::new_box_query(std::string(kField), box.min_latitude, box.max_latitude,
                                                box.min_longitude, box.max_longitude);
        ASSERT_TRUE(query.has_value()) << query.error().to_string();
        EXPECT_EQ(hits_of(searcher, query.value()),
                  expected(box.min_latitude, box.max_latitude, box.min_longitude, box.max_longitude))
                << box.min_latitude << ".." << box.max_latitude << " / " << box.min_longitude << ".."
                << box.max_longitude;
    }
}

TEST(LatLonPointBoxQueryTest, ABoxAcrossTheAntimeridianIsTwoBoxes) {
    ByteBuffersDirectory directory;
    build(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // -175 .. 175 the "wrong way round" is the whole world except that band, so
    // the antimeridian itself is inside it.
    auto query = LatLonPoint::new_box_query(std::string(kField), -90.0, 90.0, 175.0, -175.0);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const std::vector<int32_t> hits = hits_of(searcher, query.value());
    EXPECT_EQ(hits, expected(-90.0, 90.0, 175.0, -175.0));
    EXPECT_FALSE(hits.empty());
}

TEST(LatLonPointBoxQueryTest, DegenerateBoxesMatchNothing) {
    ByteBuffersDirectory directory;
    build(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // A box whose lower latitude is the north pole, and one whose longitudes are
    // both 180: the reference implementation answers "no documents" for both.
    auto pole = LatLonPoint::new_box_query(std::string(kField), 90.0, 90.0, -10.0, 10.0);
    ASSERT_TRUE(pole.has_value()) << pole.error().to_string();
    EXPECT_TRUE(hits_of(searcher, pole.value()).empty());

    auto meridian = LatLonPoint::new_box_query(std::string(kField), -90.0, 90.0, 180.0, 180.0);
    ASSERT_TRUE(meridian.has_value()) << meridian.error().to_string();
    EXPECT_TRUE(hits_of(searcher, meridian.value()).empty());
}

TEST(LatLonPointBoxQueryTest, A180LowerBoundWrapsAround) {
    ByteBuffersDirectory directory;
    build(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // min_longitude = 180 with a smaller max_longitude means "from -180 to
    // max_longitude", not a box across the antimeridian.
    auto query = LatLonPoint::new_box_query(std::string(kField), -90.0, 90.0, 180.0, -170.0);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(hits_of(searcher, query.value()), expected(-90.0, 90.0, -180.0, -170.0));
}
