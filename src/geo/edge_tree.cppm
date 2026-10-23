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

export module lucene_core:edge_tree;

import :types;
import std;

import :error;

namespace lucene {

/// A balanced interval tree over the edges of a line, polygon, or rectangle.
///
/// Port of `EdgeTree` (`core/geo/edge_tree.rs`).
///
/// Each node holds one edge `[x1, y1]`-`[x2, y2]`, keyed by `low` (the smaller y
/// of the edge) and carrying `max` (the largest y of the edge or anything below
/// it), so a query can prune whole subtrees that cannot reach the area of
/// interest.
///
/// **Deviation:** the point-in-box test uses the shared
/// `component2d_contains_point` helper instead of the reference implementation's
/// `Rectangle::contains_point`, which lives on a shape this module must not
/// depend on.
export class EdgeTree {
public:
    /// The results of [`contains_pn_poly`]: not crossed, crossed, on an edge.
    static constexpr uint8_t kFalse = 0x00;
    static constexpr uint8_t kTrue = 0x01;
    static constexpr uint8_t kOnEdge = 0x02;

    EdgeTree(double x1, double y1, double x2, double y2, double low, double max);

    [[nodiscard]] double x1() const noexcept { return x1_; }
    [[nodiscard]] double y1() const noexcept { return y1_; }
    [[nodiscard]] double x2() const noexcept { return x2_; }
    [[nodiscard]] double y2() const noexcept { return y2_; }
    [[nodiscard]] double low() const noexcept { return low_; }
    [[nodiscard]] double max() const noexcept { return max_; }
    [[nodiscard]] const EdgeTree *left() const noexcept { return left_.get(); }
    [[nodiscard]] const EdgeTree *right() const noexcept { return right_.get(); }

    /// Whether the point is inside the closed polygon the edges describe.
    [[nodiscard]] bool contains(double x, double y) const;

    /// `kFalse` if the point crosses this subtree an even number of times,
    /// `kTrue` if an odd number, `kOnEdge` if it lies on one of the edges.
    ///
    /// Port of the classic PNPOLY test (W. Randolph Franklin, BSD licensed).
    [[nodiscard]] uint8_t contains_pn_poly(double x, double y) const;

    /// Whether the point lies on one of the edges.
    [[nodiscard]] bool is_point_online(double x, double y) const;

    /// Whether the triangle crosses any edge of this subtree.
    [[nodiscard]] bool crosses_triangle(double min_x, double max_x, double min_y, double max_y, double ax, double ay,
                                        double bx, double by, double cx, double cy, bool include_boundary) const;

    /// Whether the box crosses any edge of this subtree.
    [[nodiscard]] bool crosses_box(double min_x, double max_x, double min_y, double max_y,
                                   bool include_boundary) const;

    /// Whether the line `a`-`b` crosses any edge of this subtree.
    [[nodiscard]] bool crosses_line(double min_x, double max_x, double min_y, double max_y, double a2x, double a2y,
                                    double b2x, double b2y, bool include_boundary) const;

    /// Builds the tree from the vertices of a polyline; `x` and `y` must have the
    /// same length, and at least two points.
    [[nodiscard]] static Result<EdgeTree> create(std::span<const double> x, std::span<const double> y);

private:
    /// Builds the subtree over the sorted range `[low, high]` (inclusive).
    static std::optional<EdgeTree> create_from_edges(std::vector<EdgeTree> &edges, size_t low, size_t high);

    double y1_{0};
    double y2_{0};
    double x1_{0};
    double x2_{0};
    /// The smallest y of this edge.
    double low_{0};
    /// The largest y of this edge or of any child.
    double max_{0};
    std::unique_ptr<EdgeTree> left_;
    std::unique_ptr<EdgeTree> right_;
};

} // namespace lucene
