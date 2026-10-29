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

export module lucene_core:lat_lon_bounding_box;

import :indexable_field;
import :query;
import :range_field_query;
import :types;

import std;

namespace lucene {

/// An indexed two-dimension latitude/longitude bounding box field.
///
/// Port of `LatLonBoundingBox` (`sandbox/document/`).  A box is indexed as four
/// `LatLonPoint`-encoded values (`minLat, minLon` then `maxLat, maxLon`), so the
/// queries are [`RangeFieldQuery`]s: intersects, within, contains and crosses.
///
/// The reference implementation's restrictions apply: no dateline wrapping, so a
/// box may not span the antimeridian.
///
/// **Deviation:** like the other range fields of this port, the factory returns a
/// plain [`Field`] and the query factories return `std::shared_ptr<Query>`.
export struct LatLonBoundingBox {
    /// The bytes of one dimension, the same as [`LatLonPoint`](lucene_core:lat_lon_point).
    static constexpr size_t BYTES = sizeof(int32_t);

    /// Indexes the box `[min_lat, min_lon] .. [max_lat, max_lon]` (degrees).
    [[nodiscard]] static Result<Field> create(std::string name, double min_lat, double min_lon, double max_lat,
                                              double max_lon);

    /// The four encoded coordinates the index stores.
    [[nodiscard]] static Result<std::vector<uint8_t>> encode(double min_lat, double min_lon, double max_lat,
                                                             double max_lon);

    /// Validates the box: `minLon <= maxLon`, `minLat <= maxLat`, and each value in
    /// range.
    [[nodiscard]] static VoidResult check_args(double min_lat, double min_lon, double max_lat, double max_lon);

    // ---- queries ----

    /// Boxes that intersect the given box.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_intersects_query(std::string field, double min_lat,
                                                                            double min_lon, double max_lat,
                                                                            double max_lon);
    /// Boxes that lie inside the given box.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_within_query(std::string field, double min_lat,
                                                                        double min_lon, double max_lat,
                                                                        double max_lon);
    /// Boxes that contain the given box.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_contains_query(std::string field, double min_lat,
                                                                          double min_lon, double max_lat,
                                                                          double max_lon);
    /// Boxes that cross the boundary of the given box.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_crosses_query(std::string field, double min_lat,
                                                                         double min_lon, double max_lat,
                                                                         double max_lon);

    /// `LatLonBoundingBox <field:[minLat,minLon],[maxLat,maxLon]>`.
    [[nodiscard]] static std::string to_string(std::span<const uint8_t> encoded);

private:
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_relation_query(std::string field, double min_lat,
                                                                          double min_lon, double max_lat,
                                                                          double max_lon, QueryType query_type);
};

} // namespace lucene
