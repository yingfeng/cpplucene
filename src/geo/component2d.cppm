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

export module lucene_core:geo_component2d;

import :types;
import std;

import :error;
import :point_values;

namespace lucene {

/// The relationship a query shape has with a triangle of the indexed mesh.
///
/// Port of `WithinRelation` (`core/geo/component2d.rs`).
export enum class WithinRelation : uint8_t {
    /// The shape is a candidate for "within": it either lies entirely inside the
    /// triangle or touches only edges that do not belong to the original shape.
    Candidate = 0,
    /// The shape touches an edge of the original shape, or a triangle point is
    /// inside the shape.
    NotWithin = 1,
    /// The shape and the triangle do not overlap.
    Disjoint = 2,
};

/// A 2D geometry that can relate itself to bounding boxes, lines, and triangles.
///
/// Port of the `Component2D` trait (`core/geo/component2d.rs`).
///
/// The point index walks its cells and asks the query shape about each of them,
/// so every concrete geometry (point, line, rectangle, circle, polygon, and the
/// cartesian-product shapes built from them) implements this interface.
///
/// **Deviation:** the reference implementation wraps two `Component2D`s in an
/// enum (`Component2DEnum2`) so a test can drive a lat/lon and an XY shape
/// through the same assertions; the port has no such tests, so the enum is not
/// ported.
export class Component2D {
public:
    virtual ~Component2D() = default;

    /// The smallest X of the component's bounding box.
    [[nodiscard]] virtual double min_x() const = 0;
    /// The largest X of the component's bounding box.
    [[nodiscard]] virtual double max_x() const = 0;
    /// The smallest Y of the component's bounding box.
    [[nodiscard]] virtual double min_y() const = 0;
    /// The largest Y of the component's bounding box.
    [[nodiscard]] virtual double max_y() const = 0;

    /// Whether the component contains the point `(x, y)`.
    [[nodiscard]] virtual bool contains(double x, double y) const = 0;

    /// How the component relates to the cell `[min_x, max_x] x [min_y, max_y]`.
    [[nodiscard]] virtual Result<Relation> relate(double min_x, double max_x, double min_y, double max_y) const = 0;

    /// Whether the component intersects the line `a`-`b`, whose bounding box is
    /// passed in for the implementations that need it.
    [[nodiscard]] virtual bool intersects_line(double min_x, double max_x, double min_y, double max_y, double a_x,
                                               double a_y, double b_x, double b_y) const = 0;

    /// Whether the component intersects the triangle `a`-`b`-`c`.
    [[nodiscard]] virtual bool intersects_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                                   double a_y, double b_x, double b_y, double c_x,
                                                   double c_y) const = 0;

    /// Whether the component fully contains the line `a`-`b`.
    [[nodiscard]] virtual bool contains_line(double min_x, double max_x, double min_y, double max_y, double a_x,
                                             double a_y, double b_x, double b_y) const = 0;

    /// Whether the component fully contains the triangle `a`-`b`-`c`.
    [[nodiscard]] virtual bool contains_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                                 double a_y, double b_x, double b_y, double c_x,
                                                 double c_y) const = 0;

    /// The within relation of the component with the point `(x, y)`.
    [[nodiscard]] virtual Result<WithinRelation> within_point(double x, double y) const = 0;

    /// The within relation of the component with the line `a`-`b`; `ab` is
    /// `true` when that line belongs to the original shape.
    [[nodiscard]] virtual Result<WithinRelation> within_line(double min_x, double max_x, double min_y, double max_y,
                                                             double a_x, double a_y, bool ab, double b_x,
                                                             double b_y) const = 0;

    /// The within relation of the component with the triangle `a`-`b`-`c`; the
    /// flags say which of the triangle's edges belong to the original shape.
    [[nodiscard]] virtual Result<WithinRelation> within_triangle(double min_x, double max_x, double min_y, double max_y,
                                                                 double a_x, double a_y, bool ab, double b_x, double b_y,
                                                                 bool bc, double c_x, double c_y, bool ca) const = 0;

    // ---- convenience wrappers that derive the bounding box ----

    [[nodiscard]] bool intersects_line_values(double a_x, double a_y, double b_x, double b_y) const;
    [[nodiscard]] bool intersects_triangle_values(double a_x, double a_y, double b_x, double b_y, double c_x,
                                                  double c_y) const;
    [[nodiscard]] bool contains_line_values(double a_x, double a_y, double b_x, double b_y) const;
    [[nodiscard]] bool contains_triangle_values(double a_x, double a_y, double b_x, double b_y, double c_x,
                                                double c_y) const;
    [[nodiscard]] Result<WithinRelation> within_line_values(double a_x, double a_y, bool ab, double b_x,
                                                            double b_y) const;
    [[nodiscard]] Result<WithinRelation> within_triangle_values(double a_x, double a_y, bool ab, double b_x, double b_y,
                                                                bool bc, double c_x, double c_y, bool ca) const;
};

/// Whether the two bounding boxes do not overlap.
[[nodiscard]] bool component2d_disjoint(double min_x1, double max_x1, double min_y1, double max_y1, double min_x2,
                                        double max_x2, double min_y2, double max_y2);

/// Whether the first bounding box lies inside the second one.
[[nodiscard]] bool component2d_within(double min_x1, double max_x1, double min_y1, double max_y1, double min_x2,
                                      double max_x2, double min_y2, double max_y2);

/// Whether the box `[min_x, max_x] x [min_y, max_y]` contains `(x, y)`.
[[nodiscard]] bool component2d_contains_point(double x, double y, double min_x, double max_x, double min_y,
                                              double max_y);

/// Whether `(x, y)` lies inside the triangle `a`-`b`-`c`, using the winding order
/// method.  The triangle's bounding box is passed in so the cheap rejection can
/// happen first.
[[nodiscard]] bool component2d_point_in_triangle(double min_x, double max_x, double min_y, double max_y, double x,
                                                 double y, double a_x, double a_y, double b_x, double b_y, double c_x,
                                                 double c_y);

} // namespace lucene
