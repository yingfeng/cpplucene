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

export module lucene_core:component_tree;

import :types;
import std;

import :error;
import :geo_component2d;
import :point_values;

namespace lucene {

/// A set of components, arranged as a k-d tree so the point index can prune
/// whole groups of shapes at once.
///
/// Port of `ComponentTree<T>` (`core/geo/component_tree.rs`).
///
/// **Deviation:** the reference implementation is generic over the component type
/// (`ComponentTree<Rectangle2D>`, `ComponentTree<Polygon2D>`, ...); the port holds
/// `std::shared_ptr<Component2D>` so one instantiation serves every shape, and a
/// component tree can mix shape kinds.
export class ComponentTree final : public Component2D {
public:
    /// Builds the tree over `components`, which must not be empty.
    [[nodiscard]] static Result<std::shared_ptr<ComponentTree>> create(std::vector<std::shared_ptr<Component2D>> components);

    [[nodiscard]] const Component2D &component() const noexcept { return *component_; }
    [[nodiscard]] bool split_x() const noexcept { return split_x_; }
    [[nodiscard]] const ComponentTree *left() const noexcept { return left_.get(); }
    [[nodiscard]] const ComponentTree *right() const noexcept { return right_.get(); }

    // ---- Component2D ----

    [[nodiscard]] double min_x() const override { return min_x_; }
    [[nodiscard]] double max_x() const override { return max_x_; }
    [[nodiscard]] double min_y() const override { return min_y_; }
    [[nodiscard]] double max_y() const override { return max_y_; }

    [[nodiscard]] bool contains(double x, double y) const override;

    [[nodiscard]] Result<Relation> relate(double min_x, double max_x, double min_y, double max_y) const override;

    [[nodiscard]] bool intersects_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                       double b_x, double b_y) const override;

    [[nodiscard]] bool intersects_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                           double a_y, double b_x, double b_y, double c_x, double c_y) const override;

    [[nodiscard]] bool contains_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                     double b_x, double b_y) const override;

    [[nodiscard]] bool contains_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                         double b_x, double b_y, double c_x, double c_y) const override;

    [[nodiscard]] Result<WithinRelation> within_point(double x, double y) const override;

    [[nodiscard]] Result<WithinRelation> within_line(double min_x, double max_x, double min_y, double max_y, double a_x,
                                                     double a_y, bool ab, double b_x, double b_y) const override;

    [[nodiscard]] Result<WithinRelation> within_triangle(double min_x, double max_x, double min_y, double max_y,
                                                         double a_x, double a_y, bool ab, double b_x, double b_y,
                                                         bool bc, double c_x, double c_y, bool ca) const override;

private:
    ComponentTree(std::shared_ptr<Component2D> component, bool split_x);

    /// Consumes `component_count` components starting at `next` (the median first,
    /// then the two halves) and builds the subtree.
    [[nodiscard]] static Result<std::unique_ptr<ComponentTree>> create_subtree(
            std::span<std::shared_ptr<Component2D>> components, size_t &next, size_t component_count, bool split_x);

    /// Whether a query that reaches `limit` on the split axis can descend to the
    /// right child.
    [[nodiscard]] bool right_may_overlap(double limit) const;

    double min_y_{0};
    double max_y_{0};
    double min_x_{0};
    double max_x_{0};
    std::unique_ptr<ComponentTree> left_;
    std::unique_ptr<ComponentTree> right_;
    /// Which axis this node splits on.
    bool split_x_{false};
    std::shared_ptr<Component2D> component_;
};

} // namespace lucene
