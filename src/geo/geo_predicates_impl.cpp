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

module lucene_core:geo_predicates.impl;

import :geo_predicates;

import std;

import :core_helper;
import :error;
import :geo_component2d;
import :geo_encoding_utils;
import :geo_utils;
import :point_values;
import :sloppy_math;
import :types;

namespace lucene {

namespace {

/// The bit pattern of `value` shifted so unsigned order is numeric order.
[[nodiscard]] uint32_t sortable_bits(int32_t value) noexcept {
    return static_cast<uint32_t>(value) ^ 0x8000'0000U;
}

/// The number of bits to drop so `[a, b]` fits in [`Grid::ARITY`] slots.
[[nodiscard]] uint32_t compute_shift(int64_t a, int64_t b) noexcept {
    for (uint32_t shift = 1;; ++shift) {
        const int64_t delta = static_cast<int64_t>(static_cast<uint64_t>(b) >> shift) -
                              static_cast<int64_t>(static_cast<uint64_t>(a) >> shift);
        if (delta >= 0 && delta < Grid::ARITY) {
            return shift;
        }
    }
}

/// Builds the grid over `[shape_min_lat, shape_max_lat] x [shape_min_lon, shape_max_lon]`,
/// asking `box_to_relation` for the relation of every sub-box.
[[nodiscard]] Result<Grid> create_sub_boxes(double shape_min_lat, double shape_max_lat, double shape_min_lon,
                                           double shape_max_lon,
                                           const std::function<Result<Relation>(const GeoBox &)> &box_to_relation) {
    auto min_lat = GeoEncodingUtils::encode_latitude_ceil(shape_min_lat);
    if (!min_lat.has_value()) {
        return std::unexpected(std::move(min_lat.error()));
    }
    auto max_lat = GeoEncodingUtils::encode_latitude(shape_max_lat);
    if (!max_lat.has_value()) {
        return std::unexpected(std::move(max_lat.error()));
    }
    auto min_lon = GeoEncodingUtils::encode_longitude_ceil(shape_min_lon);
    if (!min_lon.has_value()) {
        return std::unexpected(std::move(min_lon.error()));
    }
    auto max_lon = GeoEncodingUtils::encode_longitude(shape_max_lon);
    if (!max_lon.has_value()) {
        return std::unexpected(std::move(max_lon.error()));
    }

    if (max_lat.value() < min_lat.value() ||
        (shape_max_lon >= shape_min_lon && max_lon.value() < min_lon.value())) {
        return Grid::create(1, 1, 0, 0, 0, 0, {});
    }

    const int64_t min_lat2 = static_cast<int64_t>(min_lat.value()) - std::numeric_limits<int32_t>::min();
    const int64_t max_lat2 = static_cast<int64_t>(max_lat.value()) - std::numeric_limits<int32_t>::min();
    const uint32_t lat_shift = compute_shift(min_lat2, max_lat2);
    const int32_t lat_base = static_cast<int32_t>(static_cast<uint64_t>(min_lat2) >> lat_shift);
    const int32_t max_lat_delta =
            static_cast<int32_t>(static_cast<uint64_t>(max_lat2) >> lat_shift) - lat_base + 1;

    const int64_t min_lon2 = static_cast<int64_t>(min_lon.value()) - std::numeric_limits<int32_t>::min();
    int64_t max_lon2 = static_cast<int64_t>(max_lon.value()) - std::numeric_limits<int32_t>::min();
    if (shape_max_lon < shape_min_lon) {
        max_lon2 += int64_t{1} << 32;
    }
    const uint32_t lon_shift = compute_shift(min_lon2, max_lon2);
    const int32_t lon_base = static_cast<int32_t>(static_cast<uint64_t>(min_lon2) >> lon_shift);
    const int32_t max_lon_delta =
            static_cast<int32_t>(static_cast<uint64_t>(max_lon2) >> lon_shift) - lon_base + 1;

    std::vector<uint8_t> relations(static_cast<size_t>(max_lat_delta) * static_cast<size_t>(max_lon_delta), 0);
    for (int32_t i = 0; i < max_lat_delta; ++i) {
        for (int32_t j = 0; j < max_lon_delta; ++j) {
            // The reference implementation shifts with wrapping semantics; unsigned
            // arithmetic reproduces that without signed overflow.
            const int32_t box_min_lat = static_cast<int32_t>(
                    (static_cast<uint32_t>(lat_base) + static_cast<uint32_t>(i)) * (uint32_t{1} << lat_shift) +
                    0x8000'0000U);
            const int32_t box_min_lon = static_cast<int32_t>(
                    (static_cast<uint32_t>(lon_base) + static_cast<uint32_t>(j)) * (uint32_t{1} << lon_shift) +
                    0x8000'0000U);
            const int32_t box_max_lat = static_cast<int32_t>(static_cast<uint32_t>(box_min_lat) +
                                                             (uint32_t{1} << lat_shift) - 1U);
            const int32_t box_max_lon = static_cast<int32_t>(static_cast<uint32_t>(box_min_lon) +
                                                             (uint32_t{1} << lon_shift) - 1U);
            const GeoBox box{GeoEncodingUtils::decode_latitude(box_min_lat), GeoEncodingUtils::decode_latitude(box_max_lat),
                             GeoEncodingUtils::decode_longitude(box_min_lon),
                             GeoEncodingUtils::decode_longitude(box_max_lon)};
            auto relation = box_to_relation(box);
            if (!relation.has_value()) {
                return std::unexpected(std::move(relation.error()));
            }
            relations[static_cast<size_t>(i) * static_cast<size_t>(max_lon_delta) + static_cast<size_t>(j)] =
                    static_cast<uint8_t>(relation_ordinal(relation.value()));
        }
    }

    return Grid::create(lat_shift, lon_shift, lat_base, lon_base, max_lat_delta, max_lon_delta, std::move(relations));
}

} // namespace

// ---- GeoBox ----

Result<GeoBox> GeoBox::from_point_distance(double center_lat, double center_lon, double radius_meters) {
    if (auto checked = GeoUtils::check_latitude(center_lat); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = GeoUtils::check_longitude(center_lon); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }

    const double rad_lat = center_lat * std::numbers::pi / 180.0;
    const double rad_lon = center_lon * std::numbers::pi / 180.0;

    const double rad_distance = (radius_meters + 7E-2) / GeoUtils::EARTH_MEAN_RADIUS_METERS;
    double min_lat = rad_lat - rad_distance;
    double max_lat = rad_lat + rad_distance;
    double min_lon = 0;
    double max_lon = 0;

    if (min_lat > GeoUtils::MIN_LAT_RADIANS && max_lat < GeoUtils::MAX_LAT_RADIANS) {
        const double delta_lon = SloppyMath::asin(GeoUtils::sloppy_sin(rad_distance) / SloppyMath::cos(rad_lat));

        double min_lon_radians = rad_lon - delta_lon;
        if (min_lon_radians < GeoUtils::MIN_LON_RADIANS) {
            min_lon_radians += 2.0 * std::numbers::pi;
        }
        double max_lon_radians = rad_lon + delta_lon;
        if (max_lon_radians > GeoUtils::MAX_LON_RADIANS) {
            max_lon_radians -= 2.0 * std::numbers::pi;
        }
        min_lon = min_lon_radians;
        max_lon = max_lon_radians;
    } else {
        // A pole is within the distance.
        min_lat = CoreHelper::max_f64(min_lat, GeoUtils::MIN_LAT_RADIANS);
        max_lat = CoreHelper::min_f64(max_lat, GeoUtils::MAX_LAT_RADIANS);
        min_lon = GeoUtils::MIN_LON_RADIANS;
        max_lon = GeoUtils::MAX_LON_RADIANS;
    }

    GeoBox box;
    box.min_lat = min_lat * 180.0 / std::numbers::pi;
    box.max_lat = max_lat * 180.0 / std::numbers::pi;
    box.min_lon = min_lon * 180.0 / std::numbers::pi;
    box.max_lon = max_lon * 180.0 / std::numbers::pi;
    return box;
}

double GeoBox::axis_lat(double center_lat, double radius_meters) {
    constexpr double pio2 = std::numbers::pi / 2.0;
    double l1 = center_lat * std::numbers::pi / 180.0;
    const double r = (radius_meters + 7E-2) / GeoUtils::EARTH_MEAN_RADIUS_METERS;

    // Within a radius of a pole the latitude *is* the pole.
    if (std::abs(l1) + r >= GeoUtils::MAX_LAT_RADIANS) {
        return center_lat >= 0.0 ? GeoUtils::MAX_LAT_INCL : GeoUtils::MIN_LAT_INCL;
    }

    // Measure the latitude as a distance from the closest pole so the spherical
    // triangle with the box meridians is a right one.
    l1 = center_lat >= 0.0 ? pio2 - l1 : l1 + pio2;

    double l2 = std::acos(std::cos(l1) / std::cos(r));
    l2 = center_lat >= 0.0 ? pio2 - l2 : l2 - pio2;
    return l2 * 180.0 / std::numbers::pi;
}

// ---- Grid ----

Result<Grid> Grid::create(uint32_t lat_shift, uint32_t lon_shift, int32_t lat_base, int32_t lon_base,
                          int32_t max_lat_delta, int32_t max_lon_delta, std::vector<uint8_t> relations) {
    if (lat_shift < 1 || lat_shift > 31) {
        return err(LuceneError::illegal_argument("lat_shift must be between 1 and 31"));
    }
    if (lon_shift < 1 || lon_shift > 31) {
        return err(LuceneError::illegal_argument("lon_shift must be between 1 and 31"));
    }
    Grid grid;
    grid.lat_shift = lat_shift;
    grid.lon_shift = lon_shift;
    grid.lat_base = lat_base;
    grid.lon_base = lon_base;
    grid.max_lat_delta = max_lat_delta;
    grid.max_lon_delta = max_lon_delta;
    grid.relations = std::move(relations);
    return grid;
}

std::optional<Relation> Grid::relation_of(int32_t lat, int32_t lon) const {
    const uint32_t lat_unsigned = sortable_bits(lat);
    const int32_t lat2 = static_cast<int32_t>(lat_unsigned >> lat_shift);
    const int32_t lat_delta = lat2 - lat_base;
    if (lat2 < lat_base || lat_delta >= max_lat_delta) {
        return std::nullopt;
    }

    int32_t lon2 = static_cast<int32_t>(sortable_bits(lon) >> lon_shift);
    if (lon2 < lon_base) {
        lon2 += static_cast<int32_t>(uint32_t{1} << (32 - lon_shift));
    }
    const int32_t lon_delta = lon2 - lon_base;
    if (lon_delta < 0 || lon_delta >= max_lon_delta) {
        return std::nullopt;
    }

    const size_t index = static_cast<size_t>(lat_delta) * static_cast<size_t>(max_lon_delta) +
                         static_cast<size_t>(lon_delta);
    if (index >= relations.size()) {
        return std::nullopt;
    }
    const int32_t ordinal = relations[index];
    switch (ordinal) {
        case 0:
            return Relation::kCellInsideQuery;
        case 1:
            return Relation::kCellOutsideQuery;
        case 2:
            return Relation::kCellCrossesQuery;
        default:
            return std::nullopt;
    }
}

// ---- DistancePredicate ----

DistancePredicate::DistancePredicate(Grid grid, double lat, double lon, double distance_key)
        : grid_(std::move(grid)), lat_(lat), lon_(lon), distance_key_(distance_key) {}

bool DistancePredicate::test(int32_t lat, int32_t lon) const {
    const auto relation = grid_.relation_of(lat, lon);
    if (!relation.has_value()) {
        return false;
    }
    if (relation.value() == Relation::kCellCrossesQuery) {
        return SloppyMath::haversin_sort_key(GeoEncodingUtils::decode_latitude(lat),
                                             GeoEncodingUtils::decode_longitude(lon), lat_, lon_) <= distance_key_;
    }
    return relation.value() == Relation::kCellInsideQuery;
}

Result<DistancePredicate> create_distance_predicate(double lat, double lon, double radius_meters) {
    auto bounding_box = GeoBox::from_point_distance(lat, lon, radius_meters);
    if (!bounding_box.has_value()) {
        return std::unexpected(std::move(bounding_box.error()));
    }
    const double axis_lat = GeoBox::axis_lat(lat, radius_meters);
    const double distance_sort_key = GeoUtils::distance_query_sort_key(radius_meters);
    const GeoBox box = bounding_box.value();

    auto grid = create_sub_boxes(box.min_lat, box.max_lat, box.min_lon, box.max_lon,
                                 [lat, lon, distance_sort_key, axis_lat](const GeoBox &sub_box) {
                                     return GeoUtils::relate(sub_box.min_lat, sub_box.max_lat, sub_box.min_lon,
                                                             sub_box.max_lon, lat, lon, distance_sort_key, axis_lat);
                                 });
    if (!grid.has_value()) {
        return std::unexpected(std::move(grid.error()));
    }
    return DistancePredicate(std::move(grid.value()), lat, lon, distance_sort_key);
}

// ---- Component2DPredicate ----

Component2DPredicate::Component2DPredicate(Grid grid, std::shared_ptr<Component2D> tree)
        : grid_(std::move(grid)), tree_(std::move(tree)) {}

bool Component2DPredicate::test(int32_t lat, int32_t lon) const {
    const auto relation = grid_.relation_of(lat, lon);
    if (!relation.has_value()) {
        return false;
    }
    if (relation.value() == Relation::kCellCrossesQuery) {
        return tree_ != nullptr && tree_->contains(GeoEncodingUtils::decode_longitude(lon),
                                                  GeoEncodingUtils::decode_latitude(lat));
    }
    return relation.value() == Relation::kCellInsideQuery;
}

Result<Component2DPredicate> create_component_predicate(std::shared_ptr<Component2D> tree) {
    if (tree == nullptr) {
        return err(LuceneError::illegal_argument("the component must not be null"));
    }
    auto grid = create_sub_boxes(tree->min_y(), tree->max_y(), tree->min_x(), tree->max_x(),
                                 [&tree](const GeoBox &sub_box) {
                                     return tree->relate(sub_box.min_lon, sub_box.max_lon, sub_box.min_lat,
                                                         sub_box.max_lat);
                                 });
    if (!grid.has_value()) {
        return std::unexpected(std::move(grid.error()));
    }
    return Component2DPredicate(std::move(grid.value()), std::move(tree));
}

} // namespace lucene
