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

export module lucene_core:lat_lon_shape;

import :types;
import std;

import :error;
import :geo_component2d;
import :geo_geometry;
import :geo_line;
import :indexable_field;
import :query;
import :shape_field;

namespace lucene {

/// Builds the indexable fields of latitude/longitude geometries.
///
/// Port of `LatLonShape` (`core/document/lat_lon_shape.rs`).
///
/// A geometry is indexed as a triangle mesh: every triangle becomes a
/// seven-dimensional point field (see
/// [`triangle_field`](lucene_core:shape_field)), and a query compares the query
/// shape against those triangles.  A point is one degenerate triangle, and a line
/// is one flat triangle per segment.
///
/// **Deviation:** the reference implementation answers a `Vec<Fields>` holder; the
/// port answers the finished [`Field`](lucene_core:indexable_field)s, because its
/// fields are values rather than mutable objects.
///
/// Coordinates are quantized like [`LatLonPoint`](lucene_core:lat_lon_point) does
/// (about 4.2E-8 degrees for the latitude, 8.4E-8 for the longitude).
export struct LatLonShape {
    /// The field that indexes the point `(lat, lon)`.
    [[nodiscard]] static Result<std::vector<Field>> create_indexable_fields(std::string field_name, double lat,
                                                                           double lon);

    /// The fields that index `line`: one flat triangle per segment, so a line with
    /// `n` vertices gives `n - 1` fields.
    [[nodiscard]] static Result<std::vector<Field>> create_indexable_fields_from_line(std::string field_name,
                                                                                     const Line &line);

    /// Matches the documents whose indexed shape satisfies `query_relation` with at
    /// least one of `geometries`.
    ///
    /// Port of `LatLonShape::new_geometry_query` (`core/document/lat_lon_shape.rs`).
    ///
    /// **Deviation:** `QueryRelation::Contains` is rejected.  It asks whether the
    /// indexed shape contains the query shape, which the reference implementation
    /// answers by inverting the query's `Component2D`; the port has no inverted
    /// component.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_geometry_query(
            std::string field, QueryRelation query_relation,
            std::vector<std::shared_ptr<const Geometry>> geometries);
};

} // namespace lucene
