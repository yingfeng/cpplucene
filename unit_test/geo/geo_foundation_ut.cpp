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

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

module lucene_core:ut.geo_foundation;

import lucene_core;

using namespace lucene;

namespace {

constexpr double kEpsilon = 1e-6;

} // namespace

TEST(SloppyMathTest, ComputesTheHaversineDistance) {
    // One degree of latitude at the equator is about 111.19 km.
    const double distance = SloppyMath::haversin_meters(0.0, 0.0, 1.0, 0.0);
    EXPECT_NEAR(distance, 111'194.9, 2.0);

    EXPECT_DOUBLE_EQ(SloppyMath::haversin_meters(12.0, 34.0, 12.0, 34.0), 0.0);
}

TEST(SloppyMathTest, ProducesSortKeysThatOrderByDistance) {
    const double near = SloppyMath::haversin_sort_key(0.0, 0.0, 1.0, 1.0);
    const double far = SloppyMath::haversin_sort_key(0.0, 0.0, 10.0, 10.0);
    EXPECT_LT(near, far);

    // The low three bits are cleared so the encoded values compare reliably.
    EXPECT_EQ(std::bit_cast<uint64_t>(near) & 0x7ULL, 0U);

    // The key decodes back to the distance it encodes.
    EXPECT_NEAR(SloppyMath::haversin_meters_from_sort_key(near), SloppyMath::haversin_meters(0.0, 0.0, 1.0, 1.0),
                kEpsilon);

    // The largest key saturates at the antipode.
    const double max_haversin =
            SloppyMath::haversin_meters_from_sort_key(std::numeric_limits<double>::max());
    EXPECT_NEAR(max_haversin, 20'015'114.0, 10.0);
}

TEST(SloppyMathTest, ApproximatesCosAndAsin) {
    for (double angle = -3.0; angle <= 3.0; angle += 0.25) {
        EXPECT_NEAR(SloppyMath::cos(angle), std::cos(angle), 1e-9) << angle;
    }
    // The table lookup is symmetric.
    EXPECT_DOUBLE_EQ(SloppyMath::cos(-1.0), SloppyMath::cos(1.0));

    for (double value = -0.95; value <= 0.95; value += 0.05) {
        EXPECT_NEAR(SloppyMath::asin(value), std::asin(value), 1e-9) << value;
    }
    // Beyond the table the polynomial branch takes over.
    EXPECT_NEAR(SloppyMath::asin(0.99), std::asin(0.99), 1e-12);
    EXPECT_DOUBLE_EQ(SloppyMath::asin(1.0), std::numbers::pi / 2.0);
    EXPECT_TRUE(std::isnan(SloppyMath::asin(1.5)));
}

TEST(GeoUtilsTest, ValidatesCoordinates) {
    EXPECT_TRUE(GeoUtils::check_latitude(90.0).has_value());
    EXPECT_TRUE(GeoUtils::check_latitude(-90.0).has_value());
    EXPECT_TRUE(GeoUtils::check_longitude(180.0).has_value());
    EXPECT_TRUE(GeoUtils::check_longitude(-180.0).has_value());

    auto bad_latitude = GeoUtils::check_latitude(90.1);
    ASSERT_FALSE(bad_latitude.has_value());
    EXPECT_EQ(bad_latitude.error().kind(), ErrorKind::kIllegalArgument);

    EXPECT_FALSE(GeoUtils::check_latitude(std::numeric_limits<double>::quiet_NaN()).has_value());
    EXPECT_FALSE(GeoUtils::check_longitude(-180.5).has_value());
}

TEST(GeoUtilsTest, OrientsThreePoints) {
    EXPECT_EQ(GeoUtils::orient(0.0, 0.0, 1.0, 0.0, 0.0, 1.0), 1);
    EXPECT_EQ(GeoUtils::orient(0.0, 0.0, 1.0, 0.0, 0.0, -1.0), -1);
    EXPECT_EQ(GeoUtils::orient(0.0, 0.0, 1.0, 1.0, 2.0, 2.0), 0);
}

TEST(GeoUtilsTest, CrossesAndOverlapsLines) {
    // A proper crossing.
    EXPECT_TRUE(GeoUtils::line_crosses_line(0.0, 0.0, 2.0, 2.0, 0.0, 2.0, 2.0, 0.0));
    EXPECT_TRUE(GeoUtils::line_crosses_line_with_boundary(0.0, 0.0, 2.0, 2.0, 0.0, 2.0, 2.0, 0.0));

    // Segments that meet at an end point: excluded by one predicate, included by the
    // other.
    EXPECT_FALSE(GeoUtils::line_crosses_line(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 2.0, 0.0));
    EXPECT_TRUE(GeoUtils::line_crosses_line_with_boundary(0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 2.0, 0.0));

    // Collinear segments overlap; the predicate only looks at collinearity, so
    // even two disjoint pieces of the same line count.
    EXPECT_TRUE(GeoUtils::line_overlap_line(0.0, 0.0, 2.0, 2.0, 1.0, 1.0, 3.0, 3.0));
    EXPECT_TRUE(GeoUtils::line_overlap_line(0.0, 0.0, 1.0, 1.0, 2.0, 2.0, 3.0, 3.0));
    EXPECT_FALSE(GeoUtils::line_overlap_line(0.0, 0.0, 1.0, 1.0, 2.0, 3.0, 3.0, 4.0));
}

TEST(GeoUtilsTest, ChecksWhetherALongitudeRangeIsWithinNinetyDegrees) {
    EXPECT_TRUE(GeoUtils::within_90_lon_degrees(0.0, -10.0, 10.0));
    EXPECT_FALSE(GeoUtils::within_90_lon_degrees(0.0, -100.0, 100.0));
    // A range that crosses the dateline is shifted before the check.
    EXPECT_TRUE(GeoUtils::within_90_lon_degrees(179.0, 170.0, -170.0));
}

TEST(GeoUtilsTest, ComputesTheDistanceQuerySortKey) {
    const double sort_key = GeoUtils::distance_query_sort_key(100.0);
    EXPECT_NEAR(SloppyMath::haversin_meters_from_sort_key(sort_key), 100.0, 0.001);

    // A radius beyond the antipodal distance saturates.
    const double saturated = GeoUtils::distance_query_sort_key(1e9);
    EXPECT_NEAR(SloppyMath::haversin_meters_from_sort_key(saturated), 20'015'114.0, 10.0);
}

TEST(GeoEncodingUtilsTest, RoundTripsLatitudeAndLongitude) {
    for (const double latitude : {-90.0, -45.5, 0.0, 12.345, 89.999, 90.0}) {
        auto encoded = GeoEncodingUtils::encode_latitude(latitude);
        ASSERT_TRUE(encoded.has_value()) << latitude;
        // Rounding down never overshoots, so the decoded value is at or below the
        // input (except at the poles, which clamp).
        const double decoded = GeoEncodingUtils::decode_latitude(encoded.value());
        if (std::abs(latitude) != 90.0) {
            EXPECT_LE(decoded, latitude + kEpsilon);
        }
        EXPECT_GE(decoded, GeoUtils::MIN_LAT_INCL);
        EXPECT_LE(decoded, GeoUtils::MAX_LAT_INCL);
    }

    auto encoded = GeoEncodingUtils::encode_longitude(12.345);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_NEAR(GeoEncodingUtils::decode_longitude(encoded.value()), 12.345, 1e-7);
}

TEST(GeoEncodingUtilsTest, RoundsCeilUpwards) {
    const double latitude = 12.345;
    auto floor_encoded = GeoEncodingUtils::encode_latitude(latitude);
    auto ceil_encoded = GeoEncodingUtils::encode_latitude_ceil(latitude);
    ASSERT_TRUE(floor_encoded.has_value());
    ASSERT_TRUE(ceil_encoded.has_value());
    EXPECT_LE(floor_encoded.value(), ceil_encoded.value());
    EXPECT_GE(GeoEncodingUtils::decode_latitude(ceil_encoded.value()), latitude - kEpsilon);

    auto lon_floor = GeoEncodingUtils::encode_longitude(-12.345);
    auto lon_ceil = GeoEncodingUtils::encode_longitude_ceil(-12.345);
    ASSERT_TRUE(lon_floor.has_value());
    ASSERT_TRUE(lon_ceil.has_value());
    EXPECT_LE(lon_floor.value(), lon_ceil.value());
}

TEST(GeoEncodingUtilsTest, RejectsOutOfRangeCoordinates) {
    EXPECT_FALSE(GeoEncodingUtils::encode_latitude(91.0).has_value());
    EXPECT_FALSE(GeoEncodingUtils::encode_longitude(-181.0).has_value());
    EXPECT_FALSE(GeoEncodingUtils::encode_latitude_ceil(91.0).has_value());
    EXPECT_FALSE(GeoEncodingUtils::encode_longitude_ceil(181.0).has_value());
}

TEST(GeoEncodingUtilsTest, DecodesFromTheSortableBytes) {
    auto encoded = GeoEncodingUtils::encode_latitude(-33.25);
    ASSERT_TRUE(encoded.has_value());
    std::vector<uint8_t> bytes(4);
    NumericUtils::int_to_sortable_bytes(encoded.value(), bytes, 0);
    EXPECT_EQ(GeoEncodingUtils::decode_latitude_from_bytes(bytes, 0),
              GeoEncodingUtils::decode_latitude(encoded.value()));

    auto lon = GeoEncodingUtils::encode_longitude(151.25);
    ASSERT_TRUE(lon.has_value());
    NumericUtils::int_to_sortable_bytes(lon.value(), bytes, 0);
    EXPECT_EQ(GeoEncodingUtils::decode_longitude_from_bytes(bytes, 0),
              GeoEncodingUtils::decode_longitude(lon.value()));
}

TEST(GeoEncodingUtilsTest, ExposesTheEncodedLongitudeEndpoints) {
    EXPECT_EQ(GeoEncodingUtils::decode_longitude(GeoEncodingUtils::min_lon_encoded()), GeoUtils::MIN_LON_INCL);
    // 180 cannot be encoded exactly, so the maximum value decodes just below it.
    EXPECT_LE(GeoEncodingUtils::decode_longitude(GeoEncodingUtils::max_lon_encoded()), GeoUtils::MAX_LON_INCL);
    EXPECT_GT(GeoEncodingUtils::decode_longitude(GeoEncodingUtils::max_lon_encoded()), 179.999);
}

TEST(Point2DTest, RelatesToCellsAndShapes) {
    const Point2D point(5.0, 5.0);
    EXPECT_EQ(point.min_x(), 5.0);
    EXPECT_EQ(point.max_y(), 5.0);
    EXPECT_TRUE(point.contains(5.0, 5.0));
    EXPECT_FALSE(point.contains(5.0, 6.0));

    EXPECT_EQ(point.relate(0.0, 10.0, 0.0, 10.0).value(), Relation::kCellCrossesQuery);
    EXPECT_EQ(point.relate(6.0, 10.0, 6.0, 10.0).value(), Relation::kCellOutsideQuery);

    // A point on a line intersects it, and lies within nothing.
    EXPECT_TRUE(point.intersects_line(5.0, 5.0, 5.0, 5.0, 0.0, 0.0, 10.0, 10.0));
    EXPECT_FALSE(point.intersects_line(0.0, 10.0, 3.0, 5.0, 0.0, 10.0, 10.0, 3.0));
    EXPECT_FALSE(point.contains_line(0.0, 10.0, 0.0, 10.0, 0.0, 0.0, 10.0, 10.0));
    EXPECT_EQ(point.within_point(5.0, 5.0).value(), WithinRelation::Candidate);
    EXPECT_EQ(point.within_point(6.0, 5.0).value(), WithinRelation::Disjoint);
}

TEST(Rectangle2DTest, RelatesToCells) {
    const Rectangle2D rectangle(2.0, 8.0, 2.0, 8.0);
    EXPECT_TRUE(rectangle.contains(5.0, 5.0));
    EXPECT_TRUE(rectangle.contains(2.0, 8.0));
    EXPECT_FALSE(rectangle.contains(1.0, 5.0));

    // The relation is from the cell's point of view: a cell inside the rectangle is
    // fully covered by the query shape.
    EXPECT_EQ(rectangle.relate(3.0, 7.0, 3.0, 7.0).value(), Relation::kCellInsideQuery);
    EXPECT_EQ(rectangle.relate(0.0, 10.0, 0.0, 10.0).value(), Relation::kCellCrossesQuery);
    EXPECT_EQ(rectangle.relate(9.0, 10.0, 9.0, 10.0).value(), Relation::kCellOutsideQuery);

    EXPECT_TRUE(rectangle.intersects_line(0.0, 10.0, 0.0, 10.0, 0.0, 5.0, 10.0, 5.0));
    EXPECT_FALSE(rectangle.intersects_line(0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0));
    EXPECT_TRUE(rectangle.contains_line(3.0, 7.0, 3.0, 7.0, 3.0, 3.0, 7.0, 7.0));
    EXPECT_FALSE(rectangle.contains_line(0.0, 10.0, 0.0, 10.0, 0.0, 0.0, 10.0, 10.0));

    // A rectangle is never a candidate for "within": it contains other shapes but
    // is not itself inside them.
    EXPECT_EQ(rectangle.within_point(5.0, 5.0).value(), WithinRelation::NotWithin);
    EXPECT_EQ(rectangle.within_point(15.0, 5.0).value(), WithinRelation::Disjoint);

    EXPECT_EQ(rectangle.to_string(), "Rectangle2D(x=2 TO 8 y=2 TO 8)");
    EXPECT_TRUE(rectangle.equals(Rectangle2D(2.0, 8.0, 2.0, 8.0)));
    EXPECT_FALSE(rectangle.equals(Rectangle2D(2.0, 8.0, 2.0, 9.0)));
}

TEST(Line2DTest, RelatesToCells) {
    const std::vector<double> xs{0.0, 10.0};
    const std::vector<double> ys{0.0, 10.0};
    auto line = Line2D::create(xs, ys);
    ASSERT_TRUE(line.has_value()) << (line ? "" : line.error().to_string());

    EXPECT_EQ(line.value().min_x(), 0.0);
    EXPECT_EQ(line.value().max_y(), 10.0);
    EXPECT_TRUE(line.value().contains(5.0, 5.0));
    EXPECT_FALSE(line.value().contains(5.0, 6.0));

    EXPECT_EQ(line.value().relate(0.0, 10.0, 0.0, 10.0).value(), Relation::kCellCrossesQuery);
    EXPECT_EQ(line.value().relate(20.0, 30.0, 20.0, 30.0).value(), Relation::kCellOutsideQuery);

    EXPECT_TRUE(line.value().intersects_line(4.0, 6.0, 0.0, 10.0, 4.0, 10.0, 6.0, 0.0));
    EXPECT_FALSE(line.value().intersects_line(0.0, 10.0, 11.0, 12.0, 0.0, 11.0, 10.0, 11.0));
    EXPECT_FALSE(line.value().contains_line(0.0, 10.0, 0.0, 10.0, 0.0, 0.0, 10.0, 10.0));

    EXPECT_EQ(line.value().within_point(5.0, 5.0).value(), WithinRelation::NotWithin);
    EXPECT_EQ(line.value().within_point(5.0, 6.0).value(), WithinRelation::Disjoint);
}

TEST(Line2DTest, RejectsInvalidVertices) {
    EXPECT_FALSE(Line2D::create(std::vector<double>{0.0, 1.0}, std::vector<double>{0.0}).has_value());
    EXPECT_FALSE(Line2D::create(std::vector<double>{0.0}, std::vector<double>{0.0}).has_value());
}

TEST(EdgeTreeTest, SplitsAPolygonIntoAnIntervalTree) {
    // A square, as a closed polyline.
    const std::vector<double> xs{0.0, 4.0, 4.0, 0.0, 0.0};
    const std::vector<double> ys{0.0, 0.0, 4.0, 4.0, 0.0};
    auto tree = EdgeTree::create(xs, ys);
    ASSERT_TRUE(tree.has_value()) << (tree ? "" : tree.error().to_string());

    EXPECT_TRUE(tree.value().contains(2.0, 2.0));
    EXPECT_FALSE(tree.value().contains(5.0, 2.0));
    EXPECT_TRUE(tree.value().is_point_online(0.0, 2.0));
    EXPECT_FALSE(tree.value().is_point_online(2.0, 2.0));

    // Even number of crossings outside, odd inside.
    EXPECT_EQ(tree.value().contains_pn_poly(2.0, 2.0) & EdgeTree::kTrue, EdgeTree::kTrue);
    EXPECT_EQ(tree.value().contains_pn_poly(5.0, 2.0), EdgeTree::kFalse);
    EXPECT_EQ(tree.value().contains_pn_poly(0.0, 2.0), EdgeTree::kOnEdge);

    // A box strictly inside the square touches no edge; one that reaches the
    // right and top edges does.
    EXPECT_FALSE(tree.value().crosses_box(1.0, 3.0, 1.0, 3.0, true));
    EXPECT_TRUE(tree.value().crosses_box(2.0, 5.0, 2.0, 5.0, true));
    EXPECT_FALSE(tree.value().crosses_box(10.0, 12.0, 10.0, 12.0, true));
    EXPECT_TRUE(tree.value().crosses_line(0.0, 10.0, -1.0, 5.0, -1.0, 2.0, 10.0, 2.0, true));
    EXPECT_TRUE(tree.value().crosses_triangle(0.0, 10.0, 0.0, 10.0, -1.0, -1.0, 5.0, 5.0, 5.0, -1.0, true));
}

TEST(EdgeTreeTest, RejectsInvalidVertices) {
    EXPECT_FALSE(EdgeTree::create(std::vector<double>{0.0, 1.0}, std::vector<double>{0.0}).has_value());
    EXPECT_FALSE(EdgeTree::create(std::vector<double>{0.0}, std::vector<double>{0.0}).has_value());
}

TEST(ComponentTreeTest, PrunesAcrossComponents) {
    std::vector<std::shared_ptr<Component2D>> components;
    components.push_back(std::make_shared<Rectangle2D>(0.0, 1.0, 0.0, 1.0));
    components.push_back(std::make_shared<Rectangle2D>(10.0, 11.0, 10.0, 11.0));
    components.push_back(std::make_shared<Rectangle2D>(20.0, 21.0, 20.0, 21.0));

    auto tree = ComponentTree::create(components);
    ASSERT_TRUE(tree.has_value()) << (tree ? "" : tree.error().to_string());

    EXPECT_EQ(tree.value()->min_x(), 0.0);
    EXPECT_EQ(tree.value()->min_y(), 0.0);
    EXPECT_EQ(tree.value()->max_x(), 21.0);
    EXPECT_EQ(tree.value()->max_y(), 21.0);

    EXPECT_TRUE(tree.value()->contains(0.5, 0.5));
    EXPECT_TRUE(tree.value()->contains(20.5, 20.5));
    EXPECT_FALSE(tree.value()->contains(5.0, 5.0));

    EXPECT_EQ(tree.value()->relate(10.0, 11.0, 10.0, 11.0).value(), Relation::kCellInsideQuery);
    EXPECT_EQ(tree.value()->relate(2.0, 3.0, 2.0, 3.0).value(), Relation::kCellOutsideQuery);

    // A tree with more than one component cannot answer the "within" questions.
    auto within = tree.value()->within_point(0.5, 0.5);
    ASSERT_FALSE(within.has_value());
    EXPECT_EQ(within.error().kind(), ErrorKind::kIllegalArgument);

    // A single component forwards to the shape it wraps.
    std::vector<std::shared_ptr<Component2D>> single;
    single.push_back(std::make_shared<Rectangle2D>(0.0, 1.0, 0.0, 1.0));
    auto one = ComponentTree::create(single);
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one.value()->within_point(0.5, 0.5).value(), WithinRelation::NotWithin);
}

TEST(ComponentTreeTest, RejectsAnEmptyComponentList) {
    auto tree = ComponentTree::create({});
    ASSERT_FALSE(tree.has_value());
    EXPECT_EQ(tree.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(GeoBoxTest, WrapsACircleInABoundingBox) {
    auto box = GeoBox::from_point_distance(0.0, 0.0, 100'000.0);
    ASSERT_TRUE(box.has_value()) << (box ? "" : box.error().to_string());
    // 100 km is a bit under a degree of latitude.
    EXPECT_NEAR(box.value().min_lat, -0.9, 0.05);
    EXPECT_NEAR(box.value().max_lat, 0.9, 0.05);
    EXPECT_LT(box.value().min_lon, 0.0);
    EXPECT_GT(box.value().max_lon, 0.0);
    EXPECT_LT(box.value().min_lon, box.value().max_lon);

    // A circle that swallows a pole spans every longitude.
    auto polar = GeoBox::from_point_distance(89.5, 10.0, 200'000.0);
    ASSERT_TRUE(polar.has_value());
    EXPECT_EQ(polar.value().max_lat, GeoUtils::MAX_LAT_INCL);
    EXPECT_EQ(polar.value().min_lon, GeoUtils::MIN_LON_INCL);
    EXPECT_EQ(polar.value().max_lon, GeoUtils::MAX_LON_INCL);

    EXPECT_FALSE(GeoBox::from_point_distance(91.0, 0.0, 1000.0).has_value());
}

TEST(GeoBoxTest, ComputesTheAxisLatitude) {
    // For a small radius the intersection with the box meridians stays close to the
    // centre latitude.
    const double axis = GeoBox::axis_lat(45.0, 10'000.0);
    EXPECT_NEAR(axis, 45.0, 0.2);

    // At the poles it clamps.
    EXPECT_EQ(GeoBox::axis_lat(89.999, 1'000'000.0), GeoUtils::MAX_LAT_INCL);
    EXPECT_EQ(GeoBox::axis_lat(-89.999, 1'000'000.0), GeoUtils::MIN_LAT_INCL);
}

TEST(GeoPredicatesTest, TestsPointsAgainstACircle) {
    auto predicate = create_distance_predicate(0.0, 0.0, 100'000.0);
    ASSERT_TRUE(predicate.has_value()) << (predicate ? "" : predicate.error().to_string());

    const auto encode = [](double lat, double lon) {
        auto encoded_lat = GeoEncodingUtils::encode_latitude(lat);
        auto encoded_lon = GeoEncodingUtils::encode_longitude(lon);
        return std::pair<int32_t, int32_t>(encoded_lat.value(), encoded_lon.value());
    };

    const auto [center_lat, center_lon] = encode(0.0, 0.0);
    EXPECT_TRUE(predicate.value().test(center_lat, center_lon));

    // About 55 km away: inside.
    const auto [near_lat, near_lon] = encode(0.5, 0.0);
    EXPECT_TRUE(predicate.value().test(near_lat, near_lon));

    // Ten degrees away: outside.
    const auto [far_lat, far_lon] = encode(0.0, 10.0);
    EXPECT_FALSE(predicate.value().test(far_lat, far_lon));
    const auto [far_lat2, far_lon2] = encode(-10.0, 0.0);
    EXPECT_FALSE(predicate.value().test(far_lat2, far_lon2));

    // The grid only covers the circle's bounding box.
    EXPECT_GT(predicate.value().grid().max_lat_delta, 0);
    EXPECT_GT(predicate.value().grid().max_lon_delta, 0);
}

TEST(GeoPredicatesTest, TestsPointsAgainstAShape) {
    auto predicate = create_component_predicate(std::make_shared<Rectangle2D>(-10.0, 10.0, -10.0, 10.0));
    ASSERT_TRUE(predicate.has_value()) << (predicate ? "" : predicate.error().to_string());

    const auto encode = [](double lat, double lon) {
        return std::pair<int32_t, int32_t>(GeoEncodingUtils::encode_latitude(lat).value(),
                                           GeoEncodingUtils::encode_longitude(lon).value());
    };

    const auto [in_lat, in_lon] = encode(0.0, 0.0);
    EXPECT_TRUE(predicate.value().test(in_lat, in_lon));

    const auto [out_lat, out_lon] = encode(45.0, 45.0);
    EXPECT_FALSE(predicate.value().test(out_lat, out_lon));

    // A shape smaller than one sub-box still matches the points it covers.
    auto small = create_component_predicate(std::make_shared<Rectangle2D>(-0.001, 0.001, -0.001, 0.001));
    ASSERT_TRUE(small.has_value());
    const auto [tiny_lat, tiny_lon] = encode(0.0, 0.0);
    EXPECT_TRUE(small.value().test(tiny_lat, tiny_lon));

    EXPECT_FALSE(create_component_predicate(nullptr).has_value());
}
