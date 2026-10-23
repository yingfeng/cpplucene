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

export module lucene_core:geo_utils;

import :types;
import std;

import :error;
import :point_values;

namespace lucene {

/// The kind of turn three points make.
///
/// Port of `WindingOrder` (`core/geo/geo_utils.rs`).
export enum class WindingOrder : int8_t {
    /// Clockwise.
    Cw = -1,
    /// Collinear.
    Colinear = 0,
    /// Counter-clockwise.
    Ccw = 1,
};

/// The sign of a [`WindingOrder`]: `-1`, `0`, or `1`.
[[nodiscard]] constexpr int32_t winding_order_sign(WindingOrder order) noexcept {
    return static_cast<int32_t>(order);
}

/// The [`WindingOrder`] of `sign`, or an `illegal_argument` for other values.
[[nodiscard]] Result<WindingOrder> winding_order_from_sign(int32_t sign);

/// Reusable geo-spatial helpers: coordinate validation, the sloppy
/// trigonometric functions the distance queries rely on, and the orientation
/// predicates the shape code is built on.
///
/// Port of `GeoUtils` (`core/geo/geo_utils.rs`).
export struct GeoUtils {
    /// Minimum longitude value.
    static constexpr double MIN_LON_INCL = -180.0;
    /// Maximum longitude value.
    static constexpr double MAX_LON_INCL = 180.0;
    /// Minimum latitude value.
    static constexpr double MIN_LAT_INCL = -90.0;
    /// Maximum latitude value.
    static constexpr double MAX_LAT_INCL = 90.0;

    /// Minimum longitude value, in radians.
    static constexpr double MIN_LON_RADIANS = MIN_LON_INCL * std::numbers::pi / 180.0;
    /// Minimum latitude value, in radians.
    static constexpr double MIN_LAT_RADIANS = MIN_LAT_INCL * std::numbers::pi / 180.0;
    /// Maximum longitude value, in radians.
    static constexpr double MAX_LON_RADIANS = MAX_LON_INCL * std::numbers::pi / 180.0;
    /// Maximum latitude value, in radians.
    static constexpr double MAX_LAT_RADIANS = MAX_LAT_INCL * std::numbers::pi / 180.0;

    /// Mean earth axis in meters (WGS84).
    static constexpr double EARTH_MEAN_RADIUS_METERS = 6'371'008.7714;

    /// The largest error a latitude comparison may have when a distance query
    /// touches the axis of a circle.
    ///
    /// Port of `Rectangle::AXISLAT_ERROR`; it lives here because this is where
    /// the geo code uses it, and keeping it here lets the geometry shapes depend
    /// on the helpers instead of the other way round.
    static constexpr double AXISLAT_ERROR = 0.1 / EARTH_MEAN_RADIUS_METERS * 180.0 / std::numbers::pi;

    /// Fails unless `latitude` is within +/-90.
    [[nodiscard]] static VoidResult check_latitude(double latitude);
    /// Fails unless `longitude` is within +/-180.
    [[nodiscard]] static VoidResult check_longitude(double longitude);

    /// `sin(a)` via the sloppy cosine; mirrors Lucene's `sloppySin`.
    [[nodiscard]] static double sloppy_sin(double a);

    /// The largest sort key that still matches `radius` meters.
    ///
    /// Binary search over the bit patterns: any key less than or equal to the
    /// result is a query match.
    [[nodiscard]] static double distance_query_sort_key(double radius);

    /// The relation between a bounding box and a distance query.  Only valid for
    /// boxes that do not cross the dateline.
    [[nodiscard]] static Result<Relation> relate(double min_lat, double max_lat, double min_lon, double max_lon,
                                                 double lat, double lon, double distance_sort_key, double axis_lat);

    /// Whether every longitude of `[min_lon, max_lon]` is within 90 degrees of
    /// `lon`.
    [[nodiscard]] static bool within_90_lon_degrees(double lon, double min_lon, double max_lon);

    /// `1` if `a`, `b`, `c` are counter-clockwise, `-1` if clockwise, `0` if
    /// collinear.
    [[nodiscard]] static int32_t orient(double ax, double ay, double bx, double by, double cx, double cy);

    /// Whether the two line segments cross; segments that merely touch at an end
    /// point do not count.
    [[nodiscard]] static bool line_crosses_line(double a1x, double a1y, double b1x, double b1y, double a2x, double a2y,
                                                double b2x, double b2y);

    /// Whether the two line segments are collinear and overlap.
    [[nodiscard]] static bool line_overlap_line(double a1x, double a1y, double b1x, double b1y, double a2x, double a2y,
                                                double b2x, double b2y);

    /// Whether the two line segments cross, counting segments that terminate on
    /// each other (`+` and `T` shapes).
    [[nodiscard]] static bool line_crosses_line_with_boundary(double a1x, double a1y, double b1x, double b1y,
                                                              double a2x, double a2y, double b2x, double b2y);
};

} // namespace lucene
