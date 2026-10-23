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

module lucene_core:line2d.impl;

import :line2d;

import std;

import :edge_tree;
import :error;
import :geo_component2d;
import :point_values;
import :types;

namespace lucene {

Line2D::Line2D(double min_x, double max_x, double min_y, double max_y, EdgeTree tree)
        : min_x_(min_x), max_x_(max_x), min_y_(min_y), max_y_(max_y), tree_(std::move(tree)) {}

Result<Line2D> Line2D::create(std::span<const double> xs, std::span<const double> ys) {
    if (xs.size() != ys.size()) {
        return err(LuceneError::illegal_argument("x and y must be equal length"));
    }
    if (xs.empty()) {
        return err(LuceneError::illegal_argument("at least 2 geometry points required"));
    }
    auto tree = EdgeTree::create(xs, ys);
    if (!tree.has_value()) {
        return std::unexpected(std::move(tree.error()));
    }
    const auto [min_x, max_x] = std::ranges::minmax_element(xs);
    const auto [min_y, max_y] = std::ranges::minmax_element(ys);
    return Line2D(*min_x, *max_x, *min_y, *max_y, std::move(tree.value()));
}

bool Line2D::contains(double x, double y) const {
    if (component2d_contains_point(x, y, min_x_, max_x_, min_y_, max_y_)) {
        return tree_.is_point_online(x, y);
    }
    return false;
}

Result<Relation> Line2D::relate(double min_x, double max_x, double min_y, double max_y) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return Relation::kCellOutsideQuery;
    }
    if (component2d_within(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return Relation::kCellCrossesQuery;
    }
    if (tree_.crosses_box(min_x, max_x, min_y, max_y, true)) {
        return Relation::kCellCrossesQuery;
    }
    return Relation::kCellOutsideQuery;
}

bool Line2D::intersects_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y, double b_x,
                             double b_y) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return false;
    }
    return tree_.crosses_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, true);
}

bool Line2D::intersects_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                 double b_x, double b_y, double c_x, double c_y) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return false;
    }
    return component2d_point_in_triangle(min_x, max_x, min_y, max_y, tree_.x1(), tree_.y1(), a_x, a_y, b_x, b_y, c_x,
                                         c_y) ||
           tree_.crosses_triangle(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, c_x, c_y, true);
}

bool Line2D::contains_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y, double b_x,
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

bool Line2D::contains_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
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

Result<WithinRelation> Line2D::within_point(double x, double y) const {
    if (contains(x, y)) {
        return WithinRelation::NotWithin;
    }
    return WithinRelation::Disjoint;
}

Result<WithinRelation> Line2D::within_line(double min_x, double max_x, double min_y, double max_y, double a_x,
                                           double a_y, bool ab, double b_x, double b_y) const {
    if (ab && intersects_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y)) {
        return WithinRelation::NotWithin;
    }
    return WithinRelation::Disjoint;
}

Result<WithinRelation> Line2D::within_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                               double a_y, bool ab, double b_x, double b_y, bool bc, double c_x,
                                               double c_y, bool ca) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return WithinRelation::Disjoint;
    }

    WithinRelation relation = WithinRelation::Disjoint;

    if (tree_.crosses_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, true)) {
        if (ab) {
            return WithinRelation::NotWithin;
        }
        relation = WithinRelation::Candidate;
    }

    if (tree_.crosses_line(min_x, max_x, min_y, max_y, b_x, b_y, c_x, c_y, true)) {
        if (bc) {
            return WithinRelation::NotWithin;
        }
        relation = WithinRelation::Candidate;
    }

    if (tree_.crosses_line(min_x, max_x, min_y, max_y, c_x, c_y, a_x, a_y, true)) {
        if (ca) {
            return WithinRelation::NotWithin;
        }
        relation = WithinRelation::Candidate;
    }

    if (relation == WithinRelation::Candidate) {
        return WithinRelation::Candidate;
    }

    if (component2d_point_in_triangle(min_x, max_x, min_y, max_y, tree_.x1(), tree_.y1(), a_x, a_y, b_x, b_y, c_x,
                                      c_y)) {
        return WithinRelation::Candidate;
    }

    return relation;
}

} // namespace lucene
