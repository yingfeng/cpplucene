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

export module lucene_core:geo_predicates;

import :types;
import std;

import :error;
import :geo_component2d;
import :point_values;

namespace lucene {

/// The bounding box of a circle on the sphere.
export struct GeoBox {
    double min_lat{0};
    double max_lat{0};
    double min_lon{0};
    double max_lon{0};

    /// The box around `(lat, lon)` that covers `radius_meters`, using WGS-84.
    [[nodiscard]] static Result<GeoBox> from_point_distance(double center_lat, double center_lon,
                                                            double radius_meters);
    /// The latitude where the circle meets its bounding box meridians.
    [[nodiscard]] static double axis_lat(double center_lat, double radius_meters);
};

/// A grid of quantized sub-boxes, each labelled with how it relates to the query
/// shape.
///
/// Port of the private `Grid` of `core/geo/geo_encoding_utils.rs`.  It is exported
/// because the predicates below keep one by value.
export struct Grid {
    /// How many sub-boxes one dimension may have before the shift grows.
    static constexpr int32_t ARITY = 64;

    uint32_t lat_shift{0};
    uint32_t lon_shift{0};
    int32_t lat_base{0};
    int32_t lon_base{0};
    int32_t max_lat_delta{0};
    int32_t max_lon_delta{0};
    /// One relation ordinal per sub-box, row-major by latitude.
    std::vector<uint8_t> relations;

    /// Validates the shifts and the number of sub-boxes.
    [[nodiscard]] static Result<Grid> create(uint32_t lat_shift, uint32_t lon_shift, int32_t lat_base,
                                             int32_t lon_base, int32_t max_lat_delta, int32_t max_lon_delta,
                                             std::vector<uint8_t> relations);

    /// The relation of the sub-box that contains the encoded point, or `nullopt`
    /// when the point falls outside the grid.
    [[nodiscard]] std::optional<Relation> relation_of(int32_t lat, int32_t lon) const;
};

/// Decides whether a point is within a distance of a centre, cheaply: the answer
/// is precomputed per sub-box, and the exact haversine is only computed for the
/// sub-boxes the circle crosses.
///
/// Port of `DistancePredicate` (`core/geo/geo_encoding_utils.rs`).
export class DistancePredicate {
public:
    /// Whether the encoded point is within the circle.
    [[nodiscard]] bool test(int32_t lat, int32_t lon) const;

    [[nodiscard]] const Grid &grid() const noexcept { return grid_; }

private:
    friend Result<DistancePredicate> create_distance_predicate(double lat, double lon, double radius_meters);

    DistancePredicate(Grid grid, double lat, double lon, double distance_key);

    Grid grid_;
    double lat_{0};
    double lon_{0};
    double distance_key_{0};
};

/// Decides whether a point is inside a shape, cheaply: the answer is precomputed
/// per sub-box, and the shape is only consulted for the sub-boxes it crosses.
///
/// Port of `Component2DPredicate` (`core/geo/geo_encoding_utils.rs`).
///
/// **Deviation:** the reference implementation is generic over the component type;
/// the port erases it into a `shared_ptr<Component2D>`.
export class Component2DPredicate {
public:
    /// Whether the encoded point is inside the shape.
    [[nodiscard]] bool test(int32_t lat, int32_t lon) const;

    [[nodiscard]] const Grid &grid() const noexcept { return grid_; }

private:
    friend Result<Component2DPredicate> create_component_predicate(std::shared_ptr<Component2D> tree);

    Component2DPredicate(Grid grid, std::shared_ptr<Component2D> tree);

    Grid grid_;
    std::shared_ptr<Component2D> tree_;
};

/// A predicate over the circle of `radius_meters` around `(lat, lon)`.
[[nodiscard]] Result<DistancePredicate> create_distance_predicate(double lat, double lon, double radius_meters);

/// A predicate over `tree`.
[[nodiscard]] Result<Component2DPredicate> create_component_predicate(std::shared_ptr<Component2D> tree);

} // namespace lucene
