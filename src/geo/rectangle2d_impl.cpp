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

module lucene_core:rectangle2d.impl;

import :rectangle2d;

import std;

import :core_helper;
import :error;
import :geo_component2d;
import :geo_utils;
import :point_values;
import :types;

namespace lucene {

bool Rectangle2D::edges_intersect(double a_x, double a_y, double b_x, double b_y) const {
    if (CoreHelper::max_f64(a_x, b_x) < min_x_ || CoreHelper::min_f64(a_x, b_x) > max_x_ ||
        CoreHelper::min_f64(a_y, b_y) > max_y_ || CoreHelper::max_f64(a_y, b_y) < min_y_) {
        return false;
    }
    return GeoUtils::line_crosses_line_with_boundary(a_x, a_y, b_x, b_y, min_x_, max_y_, max_x_, max_y_) ||
           GeoUtils::line_crosses_line_with_boundary(a_x, a_y, b_x, b_y, max_x_, max_y_, max_x_, min_y_) ||
           GeoUtils::line_crosses_line_with_boundary(a_x, a_y, b_x, b_y, max_x_, min_y_, min_x_, min_y_) ||
           GeoUtils::line_crosses_line_with_boundary(a_x, a_y, b_x, b_y, min_x_, min_y_, min_x_, max_y_);
}

Result<Relation> Rectangle2D::relate(double min_x, double max_x, double min_y, double max_y) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return Relation::kCellOutsideQuery;
    }
    if (component2d_within(min_x, max_x, min_y, max_y, min_x_, max_x_, min_y_, max_y_)) {
        return Relation::kCellInsideQuery;
    }
    return Relation::kCellCrossesQuery;
}

bool Rectangle2D::intersects_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                  double b_x, double b_y) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return false;
    }
    return contains(a_x, a_y) || contains(b_x, b_y) || edges_intersect(a_x, a_y, b_x, b_y);
}

bool Rectangle2D::intersects_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                      double b_x, double b_y, double c_x, double c_y) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return false;
    }
    return contains(a_x, a_y) || contains(b_x, b_y) || contains(c_x, c_y) ||
           component2d_point_in_triangle(min_x, max_x, min_y, max_y, min_x_, min_y_, a_x, a_y, b_x, b_y, c_x, c_y) ||
           edges_intersect(a_x, a_y, b_x, b_y) || edges_intersect(b_x, b_y, c_x, c_y) ||
           edges_intersect(c_x, c_y, a_x, a_y);
}

bool Rectangle2D::contains_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                double b_x, double b_y) const {
    (void)a_x;
    (void)a_y;
    (void)b_x;
    (void)b_y;
    return component2d_within(min_x, max_x, min_y, max_y, min_x_, max_x_, min_y_, max_y_);
}

bool Rectangle2D::contains_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                    double b_x, double b_y, double c_x, double c_y) const {
    (void)a_x;
    (void)a_y;
    (void)b_x;
    (void)b_y;
    (void)c_x;
    (void)c_y;
    return component2d_within(min_x, max_x, min_y, max_y, min_x_, max_x_, min_y_, max_y_);
}

Result<WithinRelation> Rectangle2D::within_point(double x, double y) const {
    if (contains(x, y)) {
        return WithinRelation::NotWithin;
    }
    return WithinRelation::Disjoint;
}

Result<WithinRelation> Rectangle2D::within_line(double min_x, double max_x, double min_y, double max_y, double a_x,
                                                double a_y, bool ab, double b_x, double b_y) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return WithinRelation::Disjoint;
    }
    if (contains(a_x, a_y) || contains(b_x, b_y)) {
        return WithinRelation::NotWithin;
    }
    if (ab && edges_intersect(a_x, a_y, b_x, b_y)) {
        return WithinRelation::NotWithin;
    }
    return WithinRelation::Disjoint;
}

Result<WithinRelation> Rectangle2D::within_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                                    double a_y, bool ab, double b_x, double b_y, bool bc, double c_x,
                                                    double c_y, bool ca) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return WithinRelation::Disjoint;
    }
    if (contains(a_x, a_y) || contains(b_x, b_y) || contains(c_x, c_y)) {
        return WithinRelation::NotWithin;
    }

    WithinRelation relation = WithinRelation::Disjoint;

    if (edges_intersect(a_x, a_y, b_x, b_y)) {
        if (ab) {
            return WithinRelation::NotWithin;
        }
        relation = WithinRelation::Candidate;
    }
    if (edges_intersect(b_x, b_y, c_x, c_y)) {
        if (bc) {
            return WithinRelation::NotWithin;
        }
        relation = WithinRelation::Candidate;
    }
    if (edges_intersect(c_x, c_y, a_x, a_y)) {
        if (ca) {
            return WithinRelation::NotWithin;
        }
        relation = WithinRelation::Candidate;
    }
    if (relation == WithinRelation::Candidate) {
        return WithinRelation::Candidate;
    }
    if (component2d_point_in_triangle(min_x, max_x, min_y, max_y, min_x_, min_y_, a_x, a_y, b_x, b_y, c_x, c_y)) {
        return WithinRelation::Candidate;
    }
    return relation;
}

std::string Rectangle2D::to_string() const {
    return std::format("Rectangle2D(x={} TO {} y={} TO {})", min_x_, max_x_, min_y_, max_y_);
}

} // namespace lucene
