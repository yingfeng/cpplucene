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

module lucene_core:component_tree.impl;

import :component_tree;

import std;

import :core_helper;
import :error;
import :geo_component2d;
import :point_values;
import :types;

namespace lucene {

namespace {

/// Orders two components along one axis: by the box start, then by the box end.
[[nodiscard]] int32_t compare_components(const Component2D &left, const Component2D &right, bool split_x) {
    int32_t order = split_x ? CoreHelper::compare_f64(left.min_x(), right.min_x())
                            : CoreHelper::compare_f64(left.min_y(), right.min_y());
    if (order != 0) {
        return order;
    }
    return split_x ? CoreHelper::compare_f64(left.max_x(), right.max_x())
                   : CoreHelper::compare_f64(left.max_y(), right.max_y());
}

/// Partially sorts the components so the median ends up at `mid`, then recurses
/// into both halves with the other axis.
void partition_components(std::span<std::shared_ptr<Component2D>> components, size_t low, size_t high, bool split_x);


} // namespace

ComponentTree::ComponentTree(std::shared_ptr<Component2D> component, bool split_x)
        : min_y_(component->min_y()),
          max_y_(component->max_y()),
          min_x_(component->min_x()),
          max_x_(component->max_x()),
          split_x_(split_x),
          component_(std::move(component)) {}

bool ComponentTree::right_may_overlap(double limit) const {
    return (!split_x_ && limit >= component_->min_y()) || (split_x_ && limit >= component_->min_x());
}

bool ComponentTree::contains(double x, double y) const {
    if (y <= max_y_ && x <= max_x_) {
        if (component_->contains(x, y)) {
            return true;
        }
        if (left_ != nullptr && left_->contains(x, y)) {
            return true;
        }
        if (right_ != nullptr && right_may_overlap(y) && right_->contains(x, y)) {
            return true;
        }
    }
    return false;
}

Result<Relation> ComponentTree::relate(double min_x, double max_x, double min_y, double max_y) const {
    if (min_y <= max_y_ && min_x <= max_x_) {
        auto relation = component_->relate(min_x, max_x, min_y, max_y);
        if (!relation.has_value()) {
            return relation;
        }
        if (relation.value() != Relation::kCellOutsideQuery) {
            return relation;
        }
        if (left_ != nullptr) {
            auto left_relation = left_->relate(min_x, max_x, min_y, max_y);
            if (!left_relation.has_value()) {
                return left_relation;
            }
            if (left_relation.value() != Relation::kCellOutsideQuery) {
                return left_relation;
            }
        }
        if (right_ != nullptr && right_may_overlap(max_y)) {
            auto right_relation = right_->relate(min_x, max_x, min_y, max_y);
            if (!right_relation.has_value()) {
                return right_relation;
            }
            if (right_relation.value() != Relation::kCellOutsideQuery) {
                return right_relation;
            }
        }
    }
    return Relation::kCellOutsideQuery;
}

bool ComponentTree::intersects_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                    double b_x, double b_y) const {
    if (min_y <= max_y_ && min_x <= max_x_) {
        if (component_->intersects_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y)) {
            return true;
        }
        if (left_ != nullptr && left_->intersects_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y)) {
            return true;
        }
        if (right_ != nullptr && right_may_overlap(max_y) &&
            right_->intersects_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y)) {
            return true;
        }
    }
    return false;
}

bool ComponentTree::intersects_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                        double b_x, double b_y, double c_x, double c_y) const {
    if (min_y <= max_y_ && min_x <= max_x_) {
        if (component_->intersects_triangle(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, c_x, c_y)) {
            return true;
        }
        if (left_ != nullptr && left_->intersects_triangle(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, c_x, c_y)) {
            return true;
        }
        if (right_ != nullptr && right_may_overlap(max_y) &&
            right_->intersects_triangle(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, c_x, c_y)) {
            return true;
        }
    }
    return false;
}

bool ComponentTree::contains_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                  double b_x, double b_y) const {
    if (min_y <= max_y_ && min_x <= max_x_) {
        if (component_->contains_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y)) {
            return true;
        }
        if (left_ != nullptr && left_->contains_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y)) {
            return true;
        }
        if (right_ != nullptr && right_may_overlap(max_y) &&
            right_->contains_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y)) {
            return true;
        }
    }
    return false;
}

bool ComponentTree::contains_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                      double b_x, double b_y, double c_x, double c_y) const {
    if (min_y <= max_y_ && min_x <= max_x_) {
        if (component_->contains_triangle(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, c_x, c_y)) {
            return true;
        }
        if (left_ != nullptr && left_->contains_triangle(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, c_x, c_y)) {
            return true;
        }
        if (right_ != nullptr && right_may_overlap(max_y) &&
            right_->contains_triangle(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, c_x, c_y)) {
            return true;
        }
    }
    return false;
}

Result<WithinRelation> ComponentTree::within_point(double x, double y) const {
    if (left_ != nullptr || right_ != nullptr) {
        return err(LuceneError::illegal_argument(
                "within_point is not supported for shapes with more than one component"));
    }
    return component_->within_point(x, y);
}

Result<WithinRelation> ComponentTree::within_line(double min_x, double max_x, double min_y, double max_y, double a_x,
                                                  double a_y, bool ab, double b_x, double b_y) const {
    if (left_ != nullptr || right_ != nullptr) {
        return err(LuceneError::illegal_argument(
                "within_line is not supported for shapes with more than one component"));
    }
    return component_->within_line(min_x, max_x, min_y, max_y, a_x, a_y, ab, b_x, b_y);
}

Result<WithinRelation> ComponentTree::within_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                                      double a_y, bool ab, double b_x, double b_y, bool bc, double c_x,
                                                      double c_y, bool ca) const {
    if (left_ != nullptr || right_ != nullptr) {
        return err(LuceneError::illegal_argument(
                "within_triangle is not supported for shapes with more than one component"));
    }
    return component_->within_triangle(min_x, max_x, min_y, max_y, a_x, a_y, ab, b_x, b_y, bc, c_x, c_y, ca);
}

Result<std::shared_ptr<ComponentTree>> ComponentTree::create(
        std::vector<std::shared_ptr<Component2D>> components) {
    if (components.empty()) {
        return err(LuceneError::illegal_argument("components must not be empty"));
    }
    if (components.size() == 1) {
        // `make_shared` cannot reach the private constructor.
        return std::shared_ptr<ComponentTree>(new ComponentTree(std::move(components.front()), false));
    }

    double min_y = std::numeric_limits<double>::infinity();
    double min_x = std::numeric_limits<double>::infinity();
    for (const auto &component : components) {
        if (component == nullptr) {
            return err(LuceneError::illegal_argument("components must not contain null"));
        }
        min_y = CoreHelper::min_f64(min_y, component->min_y());
        min_x = CoreHelper::min_f64(min_x, component->min_x());
    }

    const size_t component_count = components.size();
    partition_components(components, 0, component_count - 1, false);

    size_t next = 0;
    auto root = create_subtree(components, next, component_count, false);
    if (!root.has_value()) {
        return std::unexpected(std::move(root.error()));
    }
    if (next != component_count) {
        return err(LuceneError::illegal_state("not all components were consumed during tree construction"));
    }
    root.value()->min_y_ = min_y;
    root.value()->min_x_ = min_x;
    return std::shared_ptr<ComponentTree>(std::move(root.value()));
}

namespace {

void partition_components(std::span<std::shared_ptr<Component2D>> components, size_t low, size_t high, bool split_x) {
    if (low >= high) {
        return;
    }
    const size_t mid = (low + high) >> 1;
    const auto relative_mid = static_cast<std::ptrdiff_t>(mid - low);
    auto range = components.subspan(low, high - low + 1);
    std::nth_element(range.begin(), range.begin() + relative_mid, range.end(),
                     [split_x](const std::shared_ptr<Component2D> &left, const std::shared_ptr<Component2D> &right) {
                         return compare_components(*left, *right, split_x) < 0;
                     });

    if (mid > low) {
        partition_components(components, low, mid - 1, !split_x);
    }
    if (mid < high) {
        partition_components(components, mid + 1, high, !split_x);
    }
}

} // namespace

Result<std::unique_ptr<ComponentTree>> ComponentTree::create_subtree(
        std::span<std::shared_ptr<Component2D>> components, size_t &next, size_t component_count, bool split_x) {
    if (component_count == 0) {
        return std::unique_ptr<ComponentTree>();
    }

    const size_t left_count = (component_count - 1) >> 1;
    const size_t right_count = component_count - left_count - 1;
    auto left = create_subtree(components, next, left_count, !split_x);
    if (!left.has_value()) {
        return std::unexpected(std::move(left.error()));
    }
    if (next >= components.size()) {
        return err(LuceneError::illegal_state("component missing during tree construction"));
    }
    auto component = components[next++];
    // `make_unique` cannot reach the private constructor.
    std::unique_ptr<ComponentTree> node(new ComponentTree(std::move(component), split_x));
    node->left_ = std::move(left.value());
    auto right = create_subtree(components, next, right_count, !split_x);
    if (!right.has_value()) {
        return std::unexpected(std::move(right.error()));
    }
    node->right_ = std::move(right.value());

    if (node->left_ != nullptr) {
        node->max_x_ = CoreHelper::max_f64(node->max_x_, node->left_->max_x_);
        node->max_y_ = CoreHelper::max_f64(node->max_y_, node->left_->max_y_);
    }
    if (node->right_ != nullptr) {
        node->max_x_ = CoreHelper::max_f64(node->max_x_, node->right_->max_x_);
        node->max_y_ = CoreHelper::max_f64(node->max_y_, node->right_->max_y_);
    }
    return node;
}

} // namespace lucene
