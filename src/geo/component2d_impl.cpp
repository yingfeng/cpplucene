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

module lucene_core:geo_component2d.impl;

import :geo_component2d;

import std;

import :core_helper;
import :error;
import :geo_utils;
import :point_values;
import :types;

namespace lucene {

bool Component2D::intersects_line_values(double a_x, double a_y, double b_x, double b_y) const {
    return intersects_line(CoreHelper::min_f64(a_x, b_x), CoreHelper::max_f64(a_x, b_x),
                           CoreHelper::min_f64(a_y, b_y), CoreHelper::max_f64(a_y, b_y), a_x, a_y, b_x, b_y);
}

bool Component2D::intersects_triangle_values(double a_x, double a_y, double b_x, double b_y, double c_x,
                                             double c_y) const {
    return intersects_triangle(CoreHelper::min_f64(CoreHelper::min_f64(a_x, b_x), c_x),
                               CoreHelper::max_f64(CoreHelper::max_f64(a_x, b_x), c_x),
                               CoreHelper::min_f64(CoreHelper::min_f64(a_y, b_y), c_y),
                               CoreHelper::max_f64(CoreHelper::max_f64(a_y, b_y), c_y), a_x, a_y, b_x, b_y, c_x, c_y);
}

bool Component2D::contains_line_values(double a_x, double a_y, double b_x, double b_y) const {
    return contains_line(CoreHelper::min_f64(a_x, b_x), CoreHelper::max_f64(a_x, b_x), CoreHelper::min_f64(a_y, b_y),
                         CoreHelper::max_f64(a_y, b_y), a_x, a_y, b_x, b_y);
}

bool Component2D::contains_triangle_values(double a_x, double a_y, double b_x, double b_y, double c_x,
                                           double c_y) const {
    return contains_triangle(CoreHelper::min_f64(CoreHelper::min_f64(a_x, b_x), c_x),
                             CoreHelper::max_f64(CoreHelper::max_f64(a_x, b_x), c_x),
                             CoreHelper::min_f64(CoreHelper::min_f64(a_y, b_y), c_y),
                             CoreHelper::max_f64(CoreHelper::max_f64(a_y, b_y), c_y), a_x, a_y, b_x, b_y, c_x, c_y);
}

Result<WithinRelation> Component2D::within_line_values(double a_x, double a_y, bool ab, double b_x, double b_y) const {
    return within_line(CoreHelper::min_f64(a_x, b_x), CoreHelper::max_f64(a_x, b_x), CoreHelper::min_f64(a_y, b_y),
                       CoreHelper::max_f64(a_y, b_y), a_x, a_y, ab, b_x, b_y);
}

Result<WithinRelation> Component2D::within_triangle_values(double a_x, double a_y, bool ab, double b_x, double b_y,
                                                           bool bc, double c_x, double c_y, bool ca) const {
    return within_triangle(CoreHelper::min_f64(CoreHelper::min_f64(a_x, b_x), c_x),
                           CoreHelper::max_f64(CoreHelper::max_f64(a_x, b_x), c_x),
                           CoreHelper::min_f64(CoreHelper::min_f64(a_y, b_y), c_y),
                           CoreHelper::max_f64(CoreHelper::max_f64(a_y, b_y), c_y), a_x, a_y, ab, b_x, b_y, bc, c_x,
                           c_y, ca);
}

bool component2d_disjoint(double min_x1, double max_x1, double min_y1, double max_y1, double min_x2, double max_x2,
                          double min_y2, double max_y2) {
    return max_y1 < min_y2 || min_y1 > max_y2 || max_x1 < min_x2 || min_x1 > max_x2;
}

bool component2d_within(double min_x1, double max_x1, double min_y1, double max_y1, double min_x2, double max_x2,
                        double min_y2, double max_y2) {
    return min_y2 <= min_y1 && max_y2 >= max_y1 && min_x2 <= min_x1 && max_x2 >= max_x1;
}

bool component2d_contains_point(double x, double y, double min_x, double max_x, double min_y, double max_y) {
    return x >= min_x && x <= max_x && y >= min_y && y <= max_y;
}

bool component2d_point_in_triangle(double min_x, double max_x, double min_y, double max_y, double x, double y,
                                   double a_x, double a_y, double b_x, double b_y, double c_x, double c_y) {
    if (!(x >= min_x && x <= max_x && y >= min_y && y <= max_y)) {
        return false;
    }
    const int32_t a = GeoUtils::orient(x, y, a_x, a_y, b_x, b_y);
    const int32_t b = GeoUtils::orient(x, y, b_x, b_y, c_x, c_y);
    if (a == 0 || b == 0 || (a < 0) == (b < 0)) {
        const int32_t c = GeoUtils::orient(x, y, c_x, c_y, a_x, a_y);
        return c == 0 || (c < 0) == ((b < 0) || (a < 0));
    }
    return false;
}

} // namespace lucene
