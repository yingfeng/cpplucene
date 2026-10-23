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

module lucene_core:edge_tree.impl;

import :edge_tree;

import std;

import :core_helper;
import :error;
import :geo_component2d;
import :geo_utils;
import :types;

namespace lucene {

EdgeTree::EdgeTree(double x1, double y1, double x2, double y2, double low, double max)
        : y1_(y1), y2_(y2), x1_(x1), x2_(x2), low_(low), max_(max) {}

bool EdgeTree::contains(double x, double y) const {
    return contains_pn_poly(x, y) > kFalse;
}

uint8_t EdgeTree::contains_pn_poly(double x, double y) const {
    uint8_t res = kFalse;
    if (y <= max_) {
        if ((y == y1_ && y == y2_) || ((y <= y1_ && y >= y2_) != (y >= y1_ && y <= y2_))) {
            if ((x == x1_ && x == x2_) || (((x <= x1_ && x >= x2_) != (x >= x1_ && x <= x2_)) &&
                                          GeoUtils::orient(x1_, y1_, x2_, y2_, x, y) == 0)) {
                return kOnEdge;
            }
            if ((y1_ > y) != (y2_ > y)) {
                res = x < (x2_ - x1_) * (y - y1_) / (y2_ - y1_) + x1_ ? kTrue : kFalse;
            }
        }

        if (left_ != nullptr) {
            res ^= left_->contains_pn_poly(x, y);
            if ((res & 0x02) == 0x02) {
                return kOnEdge;
            }
        }

        if (right_ != nullptr && y >= low_) {
            res ^= right_->contains_pn_poly(x, y);
            if ((res & 0x02) == 0x02) {
                return kOnEdge;
            }
        }
    }
    return res;
}

bool EdgeTree::is_point_online(double x, double y) const {
    if (y > max_) {
        return false;
    }
    const double a1x = x1_;
    const double a1y = y1_;
    const double b1x = x2_;
    const double b1y = y2_;
    const bool outside = (a1y < y && b1y < y) || (a1y > y && b1y > y) || (a1x < x && b1x < x) || (a1x > x && b1x > x);
    if (!outside && GeoUtils::orient(a1x, a1y, b1x, b1y, x, y) == 0) {
        return true;
    }
    if (left_ != nullptr && left_->is_point_online(x, y)) {
        return true;
    }
    if (right_ != nullptr && y >= low_ && right_->is_point_online(x, y)) {
        return true;
    }
    return false;
}

bool EdgeTree::crosses_triangle(double min_x, double max_x, double min_y, double max_y, double ax, double ay, double bx,
                                double by, double cx, double cy, bool include_boundary) const {
    if (min_y <= max_) {
        const double dy = y1_;
        const double ey = y2_;
        const double dx = x1_;
        const double ex = x2_;
        // Reject the edge fast when its bounding box cannot meet the query box.
        const bool outside =
                (dy < min_y && ey < min_y) || (dy > max_y && ey > max_y) || (dx < min_x && ex < min_x) || (dx > max_x && ex > max_x);
        if (!outside) {
            if (include_boundary) {
                if (GeoUtils::line_crosses_line_with_boundary(dx, dy, ex, ey, ax, ay, bx, by) ||
                    GeoUtils::line_crosses_line_with_boundary(dx, dy, ex, ey, bx, by, cx, cy) ||
                    GeoUtils::line_crosses_line_with_boundary(dx, dy, ex, ey, cx, cy, ax, ay)) {
                    return true;
                }
            } else if (GeoUtils::line_crosses_line(dx, dy, ex, ey, ax, ay, bx, by) ||
                       GeoUtils::line_crosses_line(dx, dy, ex, ey, bx, by, cx, cy) ||
                       GeoUtils::line_crosses_line(dx, dy, ex, ey, cx, cy, ax, ay)) {
                return true;
            }
        }

        if (left_ != nullptr &&
            left_->crosses_triangle(min_x, max_x, min_y, max_y, ax, ay, bx, by, cx, cy, include_boundary)) {
            return true;
        }
        if (right_ != nullptr && max_y >= low_ &&
            right_->crosses_triangle(min_x, max_x, min_y, max_y, ax, ay, bx, by, cx, cy, include_boundary)) {
            return true;
        }
    }
    return false;
}

bool EdgeTree::crosses_box(double min_x, double max_x, double min_y, double max_y, bool include_boundary) const {
    if (min_y <= max_) {
        const double cy = y1_;
        const double dy = y2_;
        const double cx = x1_;
        const double dx = x2_;

        if (component2d_contains_point(cx, cy, min_x, max_x, min_y, max_y) ||
            component2d_contains_point(dx, dy, min_x, max_x, min_y, max_y)) {
            return true;
        }
        // Reject the edge fast when its bounding box cannot meet the query box.
        const bool outside =
                (cy < min_y && dy < min_y) || (cy > max_y && dy > max_y) || (cx < min_x && dx < min_x) || (cx > max_x && dx > max_x);
        if (!outside) {
            if (include_boundary) {
                if (GeoUtils::line_crosses_line_with_boundary(cx, cy, dx, dy, min_x, min_y, max_x, min_y) ||
                    GeoUtils::line_crosses_line_with_boundary(cx, cy, dx, dy, max_x, min_y, max_x, max_y) ||
                    GeoUtils::line_crosses_line_with_boundary(cx, cy, dx, dy, max_x, max_y, min_x, max_y) ||
                    GeoUtils::line_crosses_line_with_boundary(cx, cy, dx, dy, min_x, max_y, min_x, min_y)) {
                    // Boundaries count, so a box edge that terminates on the polygon
                    // still matches.
                    return true;
                }
            } else if (GeoUtils::line_crosses_line(cx, cy, dx, dy, min_x, min_y, max_x, min_y) ||
                       GeoUtils::line_crosses_line(cx, cy, dx, dy, max_x, min_y, max_x, max_y) ||
                       GeoUtils::line_crosses_line(cx, cy, dx, dy, max_x, max_y, min_x, max_y) ||
                       GeoUtils::line_crosses_line(cx, cy, dx, dy, min_x, max_y, min_x, min_y)) {
                return true;
            }
        }

        if (left_ != nullptr && left_->crosses_box(min_x, max_x, min_y, max_y, include_boundary)) {
            return true;
        }
        if (right_ != nullptr && max_y >= low_ && right_->crosses_box(min_x, max_x, min_y, max_y, include_boundary)) {
            return true;
        }
    }
    return false;
}

bool EdgeTree::crosses_line(double min_x, double max_x, double min_y, double max_y, double a2x, double a2y, double b2x,
                            double b2y, bool include_boundary) const {
    if (min_y <= max_) {
        const double a1x = x1_;
        const double a1y = y1_;
        const double b1x = x2_;
        const double b1y = y2_;

        // Reject the edge fast when its bounding box cannot meet the query box.
        const bool outside =
                (a1y < min_y && b1y < min_y) || (a1y > max_y && b1y > max_y) || (a1x < min_x && b1x < min_x) || (a1x > max_x && b1x > max_x);
        if (!outside) {
            if (include_boundary) {
                if (GeoUtils::line_crosses_line_with_boundary(a1x, a1y, b1x, b1y, a2x, a2y, b2x, b2y)) {
                    return true;
                }
            } else if (GeoUtils::line_crosses_line(a1x, a1y, b1x, b1y, a2x, a2y, b2x, b2y)) {
                return true;
            }
        }
        if (left_ != nullptr &&
            left_->crosses_line(min_x, max_x, min_y, max_y, a2x, a2y, b2x, b2y, include_boundary)) {
            return true;
        }
        if (right_ != nullptr && max_y >= low_ &&
            right_->crosses_line(min_x, max_x, min_y, max_y, a2x, a2y, b2x, b2y, include_boundary)) {
            return true;
        }
    }
    return false;
}

Result<EdgeTree> EdgeTree::create(std::span<const double> x, std::span<const double> y) {
    if (x.size() != y.size()) {
        return err(LuceneError::illegal_argument("x and y must be equal length"));
    }
    if (x.size() < 2) {
        return err(LuceneError::illegal_argument("at least 2 geometry points required"));
    }

    std::vector<EdgeTree> edges;
    edges.reserve(x.size() - 1);
    for (size_t i = 1; i < x.size(); ++i) {
        const double x1 = x[i - 1];
        const double y1 = y[i - 1];
        const double x2 = x[i];
        const double y2 = y[i];
        edges.emplace_back(x1, y1, x2, y2, CoreHelper::min_f64(y1, y2), CoreHelper::max_f64(y1, y2));
    }

    std::ranges::sort(edges, [](const EdgeTree &left, const EdgeTree &right) {
        const int32_t low_order = CoreHelper::compare_f64(left.low(), right.low());
        if (low_order == 0) {
            return CoreHelper::compare_f64(left.max(), right.max()) < 0;
        }
        return low_order < 0;
    });

    auto root = create_from_edges(edges, 0, edges.size() - 1);
    if (!root.has_value()) {
        return err(LuceneError::illegal_state("edge tree root is missing"));
    }
    return std::move(root.value());
}

std::optional<EdgeTree> EdgeTree::create_from_edges(std::vector<EdgeTree> &edges, size_t low, size_t high) {
    if (low > high) {
        return std::nullopt;
    }

    const size_t mid = low + ((high - low) >> 1);
    EdgeTree node = std::move(edges[mid]);
    // A moved-from edge may still be visited by the sub-trees, which only ever look
    // at indices below and above `mid`, so the two ranges stay disjoint.
    if (low < mid) {
        if (auto left = create_from_edges(edges, low, mid - 1); left.has_value()) {
            node.left_ = std::make_unique<EdgeTree>(std::move(left.value()));
        }
    }
    if (mid < high) {
        if (auto right = create_from_edges(edges, mid + 1, high); right.has_value()) {
            node.right_ = std::make_unique<EdgeTree>(std::move(right.value()));
        }
    }

    if (node.left_ != nullptr) {
        node.max_ = CoreHelper::max_f64(node.max_, node.left_->max_);
    }
    if (node.right_ != nullptr) {
        node.max_ = CoreHelper::max_f64(node.max_, node.right_->max_);
    }
    return node;
}

} // namespace lucene
