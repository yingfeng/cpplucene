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

module lucene_core:lat_lon_shape.impl;

import :lat_lon_shape;

import std;

import :error;
import :geo_component2d;
import :geo_encoding_utils;
import :geo_geometry;
import :geo_line;
import :indexable_field;
import :numeric_utils;
import :query;
import :shape_field;
import :spatial_query;
import :types;

namespace lucene {

namespace {

/// The bounds of a decoded triangle, in degrees.
struct TriangleBounds {
    double min_x{0.0};
    double max_x{0.0};
    double min_y{0.0};
    double max_y{0.0};
};

TriangleBounds bounds_of(const DecodedTriangle &triangle) {
    const double a_x = GeoEncodingUtils::decode_longitude(triangle.a_x);
    const double b_x = GeoEncodingUtils::decode_longitude(triangle.b_x);
    const double c_x = GeoEncodingUtils::decode_longitude(triangle.c_x);
    const double a_y = GeoEncodingUtils::decode_latitude(triangle.a_y);
    const double b_y = GeoEncodingUtils::decode_latitude(triangle.b_y);
    const double c_y = GeoEncodingUtils::decode_latitude(triangle.c_y);
    return TriangleBounds{std::min(a_x, std::min(b_x, c_x)), std::max(a_x, std::max(b_x, c_x)),
                          std::min(a_y, std::min(b_y, c_y)), std::max(a_y, std::max(b_y, c_y))};
}

/// Answers the geometry questions of a query over shape fields.
///
/// Port of the `ShapeField` branch of `SpatialVisitorImpl`
/// (`core/document/lat_lon_shape.rs`): a cell of the point tree is answered from
/// the bounding box of the triangles in it, and a triangle that merely crosses the
/// cell is decoded and tested against the query shape.
class ShapeFieldSpatialVisitor final : public SpatialVisitor {
public:
    explicit ShapeFieldSpatialVisitor(std::shared_ptr<Component2D> component) : component_(std::move(component)) {}

    [[nodiscard]] Result<Relation> relate(std::span<const uint8_t> min_packed_value,
                                          std::span<const uint8_t> max_packed_value) const override {
        // The four index dimensions are the bounds of the triangles of the cell, with
        // the latitude first: (min_lat, min_lon, max_lat, max_lon).  Only those four
        // arrive here; the triangle itself comes with the document.
        const int32_t min_y = NumericUtils::sortable_bytes_to_int(min_packed_value, 0);
        const int32_t min_x = NumericUtils::sortable_bytes_to_int(min_packed_value, SHAPE_COORDINATE_BYTES);
        const int32_t max_y = NumericUtils::sortable_bytes_to_int(max_packed_value, 2 * SHAPE_COORDINATE_BYTES);
        const int32_t max_x = NumericUtils::sortable_bytes_to_int(max_packed_value, 3 * SHAPE_COORDINATE_BYTES);
        return component_->relate(GeoEncodingUtils::decode_longitude(min_x), GeoEncodingUtils::decode_longitude(max_x),
                                  GeoEncodingUtils::decode_latitude(min_y), GeoEncodingUtils::decode_latitude(max_y));
    }

    [[nodiscard]] Result<bool> intersects(std::span<const uint8_t> packed_value) const override {
        DecodedTriangle triangle{};
        if (auto decoded = decode_triangle(packed_value, triangle); !decoded.has_value()) {
            return std::unexpected(std::move(decoded.error()));
        }
        resolve_triangle_type(triangle);
        const TriangleBounds bounds = bounds_of(triangle);
        const double a_x = GeoEncodingUtils::decode_longitude(triangle.a_x);
        const double a_y = GeoEncodingUtils::decode_latitude(triangle.a_y);
        switch (triangle.type) {
            case DecodedTriangleType::Point:
                return component_->contains(a_x, a_y);
            case DecodedTriangleType::Line:
                return component_->intersects_line(bounds.min_x, bounds.max_x, bounds.min_y, bounds.max_y, a_x, a_y,
                                                   GeoEncodingUtils::decode_longitude(triangle.b_x),
                                                   GeoEncodingUtils::decode_latitude(triangle.b_y));
            case DecodedTriangleType::Triangle:
                return component_->intersects_triangle(
                        bounds.min_x, bounds.max_x, bounds.min_y, bounds.max_y, a_x, a_y,
                        GeoEncodingUtils::decode_longitude(triangle.b_x),
                        GeoEncodingUtils::decode_latitude(triangle.b_y),
                        GeoEncodingUtils::decode_longitude(triangle.c_x),
                        GeoEncodingUtils::decode_latitude(triangle.c_y));
        }
        return false;
    }

    [[nodiscard]] Result<bool> within(std::span<const uint8_t> packed_value) const override {
        DecodedTriangle triangle{};
        if (auto decoded = decode_triangle(packed_value, triangle); !decoded.has_value()) {
            return std::unexpected(std::move(decoded.error()));
        }
        resolve_triangle_type(triangle);
        const TriangleBounds bounds = bounds_of(triangle);
        const double a_x = GeoEncodingUtils::decode_longitude(triangle.a_x);
        const double a_y = GeoEncodingUtils::decode_latitude(triangle.a_y);
        switch (triangle.type) {
            case DecodedTriangleType::Point:
                // A point is within the query shape exactly when the shape holds it.
                return component_->contains(a_x, a_y);
            case DecodedTriangleType::Line:
                return component_->contains_line(bounds.min_x, bounds.max_x, bounds.min_y, bounds.max_y, a_x, a_y,
                                                 GeoEncodingUtils::decode_longitude(triangle.b_x),
                                                 GeoEncodingUtils::decode_latitude(triangle.b_y));
            case DecodedTriangleType::Triangle:
                return component_->contains_triangle(
                        bounds.min_x, bounds.max_x, bounds.min_y, bounds.max_y, a_x, a_y,
                        GeoEncodingUtils::decode_longitude(triangle.b_x),
                        GeoEncodingUtils::decode_latitude(triangle.b_y),
                        GeoEncodingUtils::decode_longitude(triangle.c_x),
                        GeoEncodingUtils::decode_latitude(triangle.c_y));
        }
        return false;
    }

    [[nodiscard]] Result<WithinRelation> contains(std::span<const uint8_t> /*packed_value*/) const override {
        return err(LuceneError::unsupported_operation(
                "a shape field cannot answer 'contains': it needs the inverse of the query shape"));
    }

private:
    std::shared_ptr<Component2D> component_;
};

/// Builds the visitor of a query over shape fields.
class ShapeFieldSpatial final : public SpatialQueryBase {
public:
    static Result<std::unique_ptr<ShapeFieldSpatial>> create(
            std::vector<std::shared_ptr<const Geometry>> geometries) {
        if (geometries.empty()) {
            return err(LuceneError::illegal_argument("geometries must not be empty"));
        }
        std::vector<const Geometry *> shapes;
        shapes.reserve(geometries.size());
        for (const auto &geometry : geometries) {
            if (geometry == nullptr) {
                return err(LuceneError::illegal_argument("the geometries must not be null"));
            }
            shapes.push_back(geometry.get());
        }
        auto component = create_geometry_component(shapes);
        if (!component.has_value()) {
            return std::unexpected(std::move(component.error()));
        }
        return std::unique_ptr<ShapeFieldSpatial>(new ShapeFieldSpatial(std::move(component.value())));
    }

    [[nodiscard]] Result<std::unique_ptr<SpatialVisitor>> spatial_visitor() const override {
        return std::unique_ptr<SpatialVisitor>(new ShapeFieldSpatialVisitor(component_));
    }

private:
    explicit ShapeFieldSpatial(std::shared_ptr<Component2D> component) : component_(std::move(component)) {}

    std::shared_ptr<Component2D> component_;
};

} // namespace

Result<std::vector<Field>> LatLonShape::create_indexable_fields(std::string field_name, double lat, double lon) {
    auto x = GeoEncodingUtils::encode_longitude(lon);
    if (!x.has_value()) {
        return std::unexpected(std::move(x.error()));
    }
    auto y = GeoEncodingUtils::encode_latitude(lat);
    if (!y.has_value()) {
        return std::unexpected(std::move(y.error()));
    }
    // A point is a triangle whose three vertices are the same point.
    auto field = triangle_field(field_name, x.value(), y.value(), x.value(), y.value(), x.value(), y.value());
    if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
    }
    std::vector<Field> fields;
    fields.push_back(std::move(field.value()));
    return fields;
}

Result<std::vector<Field>> LatLonShape::create_indexable_fields_from_line(std::string field_name, const Line &line) {
    if (line.num_points() < 2) {
        return err(LuceneError::illegal_argument("a line needs at least two vertices"));
    }
    std::vector<Field> fields;
    fields.reserve(line.num_points() - 1);
    for (size_t i = 0; i + 1 < line.num_points(); ++i) {
        auto first_x = GeoEncodingUtils::encode_longitude(line.lon(i));
        if (!first_x.has_value()) {
            return std::unexpected(std::move(first_x.error()));
        }
        auto first_y = GeoEncodingUtils::encode_latitude(line.lat(i));
        if (!first_y.has_value()) {
            return std::unexpected(std::move(first_y.error()));
        }
        auto second_x = GeoEncodingUtils::encode_longitude(line.lon(i + 1));
        if (!second_x.has_value()) {
            return std::unexpected(std::move(second_x.error()));
        }
        auto second_y = GeoEncodingUtils::encode_latitude(line.lat(i + 1));
        if (!second_y.has_value()) {
            return std::unexpected(std::move(second_y.error()));
        }
        // A segment is a flat triangle: both ends of the segment, then the first one
        // again, which is what the reference implementation indexes as well.
        auto field = triangle_field(field_name, first_x.value(), first_y.value(), second_x.value(), second_y.value(),
                                    first_x.value(), first_y.value());
        if (!field.has_value()) {
            return std::unexpected(std::move(field.error()));
        }
        fields.push_back(std::move(field.value()));
    }
    return fields;
}

Result<std::shared_ptr<Query>> LatLonShape::new_geometry_query(
        std::string field, QueryRelation query_relation, std::vector<std::shared_ptr<const Geometry>> geometries) {
    if (query_relation == QueryRelation::Contains) {
        return err(LuceneError::unsupported_operation(
                "a shape field cannot answer 'contains': it needs the inverse of the query shape"));
    }
    auto spatial = ShapeFieldSpatial::create(geometries);
    if (!spatial.has_value()) {
        return std::unexpected(std::move(spatial.error()));
    }
    std::shared_ptr<const SpatialQueryBase> sub(std::move(spatial.value()));
    auto query = SpatialQuery::create(std::move(field), query_relation, std::move(geometries), std::move(sub));
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::move(query.value()));
}

} // namespace lucene
