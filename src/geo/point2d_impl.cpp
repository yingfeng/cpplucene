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

module lucene_core:point2d.impl;

import :point2d;

import std;

import :error;
import :geo_component2d;
import :geo_utils;
import :point_values;
import :types;

namespace lucene {

Result<Relation> Point2D::relate(double min_x, double max_x, double min_y, double max_y) const {
    if (component2d_contains_point(x_, y_, min_x, max_x, min_y, max_y)) {
        return Relation::kCellCrossesQuery;
    }
    return Relation::kCellOutsideQuery;
}

bool Point2D::intersects_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y, double b_x,
                              double b_y) const {
    return component2d_contains_point(x_, y_, min_x, max_x, min_y, max_y) &&
           GeoUtils::orient(a_x, a_y, b_x, b_y, x_, y_) == 0;
}

bool Point2D::intersects_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                  double b_x, double b_y, double c_x, double c_y) const {
    return component2d_point_in_triangle(min_x, max_x, min_y, max_y, x_, y_, a_x, a_y, b_x, b_y, c_x, c_y);
}

bool Point2D::contains_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y, double b_x,
                            double b_y) const {
    (void)min_x;
    (void)max_x;
    (void)min_y;
    (void)max_y;
    (void)a_x;
    (void)a_y;
    (void)b_x;
    (void)b_y;
    return false;
}

bool Point2D::contains_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                double b_x, double b_y, double c_x, double c_y) const {
    (void)min_x;
    (void)max_x;
    (void)min_y;
    (void)max_y;
    (void)a_x;
    (void)a_y;
    (void)b_x;
    (void)b_y;
    (void)c_x;
    (void)c_y;
    return false;
}

Result<WithinRelation> Point2D::within_point(double x, double y) const {
    if (contains(x, y)) {
        return WithinRelation::Candidate;
    }
    return WithinRelation::Disjoint;
}

Result<WithinRelation> Point2D::within_line(double min_x, double max_x, double min_y, double max_y, double a_x,
                                            double a_y, bool ab, double b_x, double b_y) const {
    (void)ab;
    if (intersects_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y)) {
        return WithinRelation::Candidate;
    }
    return WithinRelation::Disjoint;
}

Result<WithinRelation> Point2D::within_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                               double a_y, bool ab, double b_x, double b_y, bool bc, double c_x,
                                               double c_y, bool ca) const {
    (void)ab;
    (void)bc;
    (void)ca;
    if (component2d_point_in_triangle(min_x, max_x, min_y, max_y, x_, y_, a_x, a_y, b_x, b_y, c_x, c_y)) {
        return WithinRelation::Candidate;
    }
    return WithinRelation::Disjoint;
}

} // namespace lucene
