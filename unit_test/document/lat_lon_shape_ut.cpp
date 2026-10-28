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

// `LatLonShape`: a point becomes one degenerate triangle and a line one flat
// triangle per segment, and the geometry queries find them.

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.lat_lon_shape;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kShapeField = "shape";

std::vector<int32_t> search_docs(IndexSearcher &searcher, const Query &query) {
    auto hits = searcher.search(query, 10);
    EXPECT_TRUE(hits.has_value()) << (hits.has_value() ? "" : hits.error().to_string());
    std::vector<int32_t> docs;
    if (!hits.has_value()) {
        return docs;
    }
    for (const ScoreDoc &hit : hits.value().score_docs) {
        docs.push_back(hit.doc);
    }
    return docs;
}

/// A rectangle as a polygon; the vertices are given clockwise and the first one is
/// repeated, because a polygon has to close itself.
Result<std::shared_ptr<const Geometry>> rectangle(double min_lat, double max_lat, double min_lon, double max_lon) {
    auto polygon = Polygon::create(std::vector<double>{min_lat, min_lat, max_lat, max_lat, min_lat},
                                   std::vector<double>{min_lon, max_lon, max_lon, min_lon, min_lon});
    if (!polygon.has_value()) {
        return std::unexpected(std::move(polygon.error()));
    }
    return std::shared_ptr<const Geometry>(std::make_shared<Polygon>(std::move(polygon.value())));
}

/// Builds an index with a point, a two vertex line and a three vertex line.
std::shared_ptr<DirectoryReader> build_index(Directory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    EXPECT_TRUE(writer.has_value());
    if (!writer.has_value()) {
        return nullptr;
    }

    Document point_doc;
    auto point = LatLonShape::create_indexable_fields(std::string(kShapeField), /*lat=*/0.0, /*lon=*/0.0);
    EXPECT_TRUE(point.has_value());
    if (point.has_value()) {
        for (Field &field : point.value()) {
            point_doc.add(std::make_unique<Field>(std::move(field)));
        }
    }
    EXPECT_TRUE(writer.value()->add_document(std::move(point_doc)).has_value());

    auto two_vertex_line = Line::create(std::vector<double>{0.0, 0.0}, std::vector<double>{-1.0, 1.0});
    EXPECT_TRUE(two_vertex_line.has_value());
    if (two_vertex_line.has_value()) {
        Document line_doc;
        auto fields = LatLonShape::create_indexable_fields_from_line(std::string(kShapeField), two_vertex_line.value());
        EXPECT_TRUE(fields.has_value());
        if (fields.has_value()) {
            for (Field &field : fields.value()) {
                line_doc.add(std::make_unique<Field>(std::move(field)));
            }
        }
        EXPECT_TRUE(writer.value()->add_document(std::move(line_doc)).has_value());
    }

    auto three_vertex_line = Line::create(std::vector<double>{0.0, 0.0, 0.0}, std::vector<double>{2.0, 3.0, 4.0});
    EXPECT_TRUE(three_vertex_line.has_value());
    if (three_vertex_line.has_value()) {
        Document line_doc;
        auto fields = LatLonShape::create_indexable_fields_from_line(std::string(kShapeField), three_vertex_line.value());
        EXPECT_TRUE(fields.has_value());
        EXPECT_EQ(fields.value().size(), 2U); // one triangle per segment
        if (fields.has_value()) {
            for (Field &field : fields.value()) {
                line_doc.add(std::make_unique<Field>(std::move(field)));
            }
        }
        EXPECT_TRUE(writer.value()->add_document(std::move(line_doc)).has_value());
    }

    EXPECT_TRUE(writer.value()->close().has_value());
    auto reader = DirectoryReader::open(directory);
    EXPECT_TRUE(reader.has_value());
    return reader.has_value() ? reader.value() : nullptr;
}

} // namespace

TEST(LatLonShapeTest, TheFieldTypeIsASevenDimensionalPoint) {
    auto type = shape_field_type();
    ASSERT_TRUE(type.has_value()) << type.error().to_string();
    EXPECT_EQ(type.value().point_dimension_count(), 7U);
    EXPECT_EQ(type.value().point_index_dimension_count(), 4U);
    EXPECT_EQ(type.value().point_num_bytes(), 4U);
}

TEST(LatLonShapeTest, ATranslatedTriangleDecodesBackToItsVertices) {
    auto x = GeoEncodingUtils::encode_longitude(1.5);
    ASSERT_TRUE(x.has_value());
    auto y = GeoEncodingUtils::encode_latitude(0.5);
    ASSERT_TRUE(y.has_value());

    // A point triangle.
    auto point = triangle_field(std::string(kShapeField), x.value(), y.value(), x.value(), y.value(), x.value(),
                                y.value());
    ASSERT_TRUE(point.has_value()) << point.error().to_string();
    const std::optional<std::vector<uint8_t>> point_bytes = point.value().binary_value();
    ASSERT_TRUE(point_bytes.has_value());
    ASSERT_EQ(point_bytes->size(), SHAPE_TRIANGLE_BYTES);
    DecodedTriangle decoded;
    ASSERT_TRUE(decode_triangle(*point_bytes, decoded).has_value());
    resolve_triangle_type(decoded);
    EXPECT_EQ(decoded.type, DecodedTriangleType::Point);
    EXPECT_EQ(decoded.a_x, x.value());
    EXPECT_EQ(decoded.a_y, y.value());

    // A flat triangle, which is what a line segment becomes.
    auto other_x = GeoEncodingUtils::encode_longitude(2.5);
    ASSERT_TRUE(other_x.has_value());
    auto segment = triangle_field(std::string(kShapeField), x.value(), y.value(), other_x.value(), y.value(),
                                  x.value(), y.value());
    ASSERT_TRUE(segment.has_value()) << segment.error().to_string();
    const std::optional<std::vector<uint8_t>> segment_bytes = segment.value().binary_value();
    ASSERT_TRUE(segment_bytes.has_value());
    DecodedTriangle line;
    ASSERT_TRUE(decode_triangle(*segment_bytes, line).has_value());
    resolve_triangle_type(line);
    EXPECT_EQ(line.type, DecodedTriangleType::Line);
}

TEST(LatLonShapeTest, AGeometryQueryFindsTheIndexedPointsAndLines) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(reader->num_docs().value(), 3);

    IndexSearcher searcher(reader);

    // The square around the origin holds the point, and the two vertex line runs
    // through it as well.
    auto centre = rectangle(-0.5, 0.5, -0.5, 0.5);
    ASSERT_TRUE(centre.has_value()) << centre.error().to_string();
    auto centre_query = LatLonShape::new_geometry_query(std::string(kShapeField), QueryRelation::Intersects,
                                                       {centre.value()});
    ASSERT_TRUE(centre_query.has_value()) << centre_query.error().to_string();
    EXPECT_EQ(search_docs(searcher, *centre_query.value()), (std::vector<int32_t>{0, 1}));

    // The whole world matches every document.
    auto world = rectangle(-90.0, 90.0, -180.0, 180.0);
    ASSERT_TRUE(world.has_value()) << world.error().to_string();
    auto world_query = LatLonShape::new_geometry_query(std::string(kShapeField), QueryRelation::Intersects,
                                                     {world.value()});
    ASSERT_TRUE(world_query.has_value()) << world_query.error().to_string();
    EXPECT_EQ(search_docs(searcher, *world_query.value()), (std::vector<int32_t>{0, 1, 2}));

    // A square that crosses the two vertex line, away from any vertex.
    auto on_line = rectangle(-0.5, 0.5, 0.9, 1.1);
    ASSERT_TRUE(on_line.has_value()) << on_line.error().to_string();
    auto line_query = LatLonShape::new_geometry_query(std::string(kShapeField), QueryRelation::Intersects,
                                                     {on_line.value()});
    ASSERT_TRUE(line_query.has_value()) << line_query.error().to_string();
    EXPECT_EQ(search_docs(searcher, *line_query.value()), (std::vector<int32_t>{1}));

    // A square that crosses the middle segment of the three vertex line.
    auto on_second_line = rectangle(-0.5, 0.5, 2.4, 2.6);
    ASSERT_TRUE(on_second_line.has_value()) << on_second_line.error().to_string();
    auto second_line_query = LatLonShape::new_geometry_query(std::string(kShapeField), QueryRelation::Intersects,
                                                            {on_second_line.value()});
    ASSERT_TRUE(second_line_query.has_value()) << second_line_query.error().to_string();
    EXPECT_EQ(search_docs(searcher, *second_line_query.value()), (std::vector<int32_t>{2}));

    // Nothing is indexed far away from the origin.
    auto far_away = rectangle(10.0, 11.0, 10.0, 11.0);
    ASSERT_TRUE(far_away.has_value()) << far_away.error().to_string();
    auto far_query = LatLonShape::new_geometry_query(std::string(kShapeField), QueryRelation::Intersects,
                                                    {far_away.value()});
    ASSERT_TRUE(far_query.has_value()) << far_query.error().to_string();
    EXPECT_TRUE(search_docs(searcher, *far_query.value()).empty());
}

TEST(LatLonShapeTest, ALineNeedsTwoVertices) {
    auto one_vertex = Line::create(std::vector<double>{0.0}, std::vector<double>{0.0});
    // The line itself refuses a single vertex, and a line that somehow had one would
    // be rejected as well.
    EXPECT_FALSE(one_vertex.has_value());
}
