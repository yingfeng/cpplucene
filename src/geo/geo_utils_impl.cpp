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

module lucene_core:geo_utils.impl;

import :geo_utils;

import std;

import :error;
import :point_values;
import :sloppy_math;
import :types;

namespace lucene {

namespace {

constexpr double PIO2 = std::numbers::pi / 2.0;

} // namespace

Result<WindingOrder> winding_order_from_sign(int32_t sign) {
    switch (sign) {
        case -1:
            return WindingOrder::Cw;
        case 0:
            return WindingOrder::Colinear;
        case 1:
            return WindingOrder::Ccw;
        default:
            return err(LuceneError::illegal_argument(std::format("Invalid WindingOrder sign: {}", sign)));
    }
}

VoidResult GeoUtils::check_latitude(double latitude) {
    if (std::isnan(latitude) || latitude < MIN_LAT_INCL || latitude > MAX_LAT_INCL) {
        return err(LuceneError::illegal_argument(
                std::format("invalid latitude {}; must be between {} and {}", latitude, MIN_LAT_INCL, MAX_LAT_INCL)));
    }
    return {};
}

VoidResult GeoUtils::check_longitude(double longitude) {
    if (std::isnan(longitude) || longitude < MIN_LON_INCL || longitude > MAX_LON_INCL) {
        return err(LuceneError::illegal_argument(
                std::format("invalid longitude {}; must be between {} and {}", longitude, MIN_LON_INCL, MAX_LON_INCL)));
    }
    return {};
}

double GeoUtils::sloppy_sin(double a) {
    return SloppyMath::cos(a - PIO2);
}

double GeoUtils::distance_query_sort_key(double radius) {
    constexpr double max_sort_key = std::numeric_limits<double>::max();
    const double max_haversin = SloppyMath::haversin_meters_from_sort_key(max_sort_key);
    if (radius >= max_haversin) {
        return max_haversin;
    }

    uint64_t lo = 0;
    uint64_t hi = std::bit_cast<uint64_t>(max_sort_key);

    while (lo <= hi) {
        const uint64_t mid = lo + ((hi - lo) >> 1);
        const double sort_key = std::bit_cast<double>(mid);
        const double mid_radius = SloppyMath::haversin_meters_from_sort_key(sort_key);

        if (mid_radius == radius) {
            return sort_key;
        }
        if (mid_radius > radius) {
            if (mid == 0) {
                break;
            }
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return std::bit_cast<double>(lo);
}

Result<Relation> GeoUtils::relate(double min_lat, double max_lat, double min_lon, double max_lon, double lat, double lon,
                                  double distance_sort_key, double axis_lat) {
    if (min_lon > max_lon) {
        return err(LuceneError::illegal_argument("Box crosses the dateline"));
    }

    if ((lon < min_lon || lon > max_lon) && (axis_lat + AXISLAT_ERROR < min_lat || axis_lat - AXISLAT_ERROR > max_lat)) {
        // The circle is neither fully inside the box nor crossing its axis, so the
        // four corners decide.
        if (SloppyMath::haversin_sort_key(lat, lon, min_lat, min_lon) > distance_sort_key &&
            SloppyMath::haversin_sort_key(lat, lon, min_lat, max_lon) > distance_sort_key &&
            SloppyMath::haversin_sort_key(lat, lon, max_lat, min_lon) > distance_sort_key &&
            SloppyMath::haversin_sort_key(lat, lon, max_lat, max_lon) > distance_sort_key) {
            return Relation::kCellOutsideQuery;
        }
    }

    if (within_90_lon_degrees(lon, min_lon, max_lon) &&
        SloppyMath::haversin_sort_key(lat, lon, min_lat, min_lon) <= distance_sort_key &&
        SloppyMath::haversin_sort_key(lat, lon, min_lat, max_lon) <= distance_sort_key &&
        SloppyMath::haversin_sort_key(lat, lon, max_lat, min_lon) <= distance_sort_key &&
        SloppyMath::haversin_sort_key(lat, lon, max_lat, max_lon) <= distance_sort_key) {
        return Relation::kCellInsideQuery;
    }

    return Relation::kCellCrossesQuery;
}

bool GeoUtils::within_90_lon_degrees(double lon, double min_lon, double max_lon) {
    if (max_lon <= lon - 180.0) {
        lon -= 360.0;
    } else if (min_lon >= lon + 180.0) {
        lon += 360.0;
    }
    return max_lon - lon < 90.0 && lon - min_lon < 90.0;
}

int32_t GeoUtils::orient(double ax, double ay, double bx, double by, double cx, double cy) {
    const double v1 = (bx - ax) * (cy - ay);
    const double v2 = (cx - ax) * (by - ay);
    if (v1 > v2) {
        return 1;
    }
    if (v1 < v2) {
        return -1;
    }
    return 0;
}

bool GeoUtils::line_crosses_line(double a1x, double a1y, double b1x, double b1y, double a2x, double a2y, double b2x,
                                 double b2y) {
    return orient(a2x, a2y, b2x, b2y, a1x, a1y) * orient(a2x, a2y, b2x, b2y, b1x, b1y) < 0 &&
           orient(a1x, a1y, b1x, b1y, a2x, a2y) * orient(a1x, a1y, b1x, b1y, b2x, b2y) < 0;
}

bool GeoUtils::line_overlap_line(double a1x, double a1y, double b1x, double b1y, double a2x, double a2y, double b2x,
                                 double b2y) {
    return orient(a2x, a2y, b2x, b2y, a1x, a1y) == 0 && orient(a2x, a2y, b2x, b2y, b1x, b1y) == 0 &&
           orient(a1x, a1y, b1x, b1y, a2x, a2y) == 0 && orient(a1x, a1y, b1x, b1y, b2x, b2y) == 0;
}

bool GeoUtils::line_crosses_line_with_boundary(double a1x, double a1y, double b1x, double b1y, double a2x, double a2y,
                                               double b2x, double b2y) {
    return orient(a2x, a2y, b2x, b2y, a1x, a1y) * orient(a2x, a2y, b2x, b2y, b1x, b1y) <= 0 &&
           orient(a1x, a1y, b1x, b1y, a2x, a2y) * orient(a1x, a1y, b1x, b1y, b2x, b2y) <= 0;
}

} // namespace lucene
