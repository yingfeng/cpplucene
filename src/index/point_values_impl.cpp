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

module lucene_core:point_values.impl;

import :point_values;

import :bytes_ref;
import :error;
import :types;

import std;

namespace lucene {

VoidResult intersect_with_point_tree(IntersectVisitor &visitor, PointTree &point_tree) {
    auto min = point_tree.get_min_packed_value();
    if (!min) {
        return std::unexpected(std::move(min.error()));
    }
    auto max = point_tree.get_max_packed_value();
    if (!max) {
        return std::unexpected(std::move(max.error()));
    }
    auto relation = visitor.compare(min.value().as_slice(), max.value().as_slice());
    if (!relation) {
        return std::unexpected(std::move(relation.error()));
    }

    switch (relation.value()) {
        case Relation::kCellOutsideQuery:
            // This cell is fully outside the query shape: stop recursing.
            return {};
        case Relation::kCellInsideQuery:
            // This cell is fully inside the query shape: recursively add all
            // points in this cell without filtering.
            return point_tree.visit_doc_ids(visitor);
        case Relation::kCellCrossesQuery: {
            // The cell crosses the shape boundary, or the cell fully contains
            // the query, so we fall through and do full filtering.
            auto has_child = point_tree.move_to_child();
            if (!has_child) {
                return std::unexpected(std::move(has_child.error()));
            }
            if (!has_child.value()) {
                // Leaf cell: do the filtering.
                return point_tree.visit_doc_values(visitor);
            }
            while (true) {
                if (auto visited = intersect_with_point_tree(visitor, point_tree); !visited) {
                    return visited;
                }
                auto sibling = point_tree.move_to_sibling();
                if (!sibling) {
                    return std::unexpected(std::move(sibling.error()));
                }
                if (!sibling.value()) {
                    break;
                }
            }
            auto parent = point_tree.move_to_parent();
            if (!parent) {
                return std::unexpected(std::move(parent.error()));
            }
            return {};
        }
    }
    return err(LuceneError::illegal_state("unknown point relation"));
}

Result<int64_t> estimate_point_count_with_point_tree(const IntersectVisitor &visitor, PointTree &point_tree,
                                                     int64_t upper_bound) {
    auto min = point_tree.get_min_packed_value();
    if (!min) {
        return std::unexpected(std::move(min.error()));
    }
    auto max = point_tree.get_max_packed_value();
    if (!max) {
        return std::unexpected(std::move(max.error()));
    }
    auto relation = visitor.compare(min.value().as_slice(), max.value().as_slice());
    if (!relation) {
        return std::unexpected(std::move(relation.error()));
    }

    switch (relation.value()) {
        case Relation::kCellOutsideQuery:
            // This cell is fully outside the query shape: no points added.
            return 0;
        case Relation::kCellInsideQuery: {
            // This cell is fully inside the query shape: add all points.
            auto count = point_tree.size();
            if (!count) {
                return std::unexpected(std::move(count.error()));
            }
            return static_cast<int64_t>(count.value());
        }
        case Relation::kCellCrossesQuery: {
            auto has_child = point_tree.move_to_child();
            if (!has_child) {
                return std::unexpected(std::move(has_child.error()));
            }
            if (!has_child.value()) {
                // Assume half the points matched.
                auto count = point_tree.size();
                if (!count) {
                    return std::unexpected(std::move(count.error()));
                }
                return static_cast<int64_t>((count.value() + 1) / 2);
            }
            int64_t cost = 0;
            while (true) {
                auto child_cost = estimate_point_count_with_point_tree(visitor, point_tree, upper_bound - cost);
                if (!child_cost) {
                    return std::unexpected(std::move(child_cost.error()));
                }
                cost += child_cost.value();
                if (cost >= upper_bound) {
                    break;
                }
                auto sibling = point_tree.move_to_sibling();
                if (!sibling) {
                    return std::unexpected(std::move(sibling.error()));
                }
                if (!sibling.value()) {
                    break;
                }
            }
            auto parent = point_tree.move_to_parent();
            if (!parent) {
                return std::unexpected(std::move(parent.error()));
            }
            return cost;
        }
    }
    return err(LuceneError::illegal_state("unknown point relation"));
}

VoidResult PointValues::intersect(IntersectVisitor &visitor) const {
    auto point_tree = get_point_tree();
    if (!point_tree) {
        return std::unexpected(std::move(point_tree.error()));
    }
    return intersect_with_point_tree(visitor, *point_tree.value());
}

Result<int64_t> PointValues::estimate_point_count(const IntersectVisitor &visitor) const {
    auto point_tree = get_point_tree();
    if (!point_tree) {
        return std::unexpected(std::move(point_tree.error()));
    }
    return estimate_point_count_with_point_tree(visitor, *point_tree.value(), std::numeric_limits<int64_t>::max());
}

Result<int64_t> PointValues::estimate_doc_count(const IntersectVisitor &visitor) const {
    auto estimated_point_count = estimate_point_count(visitor);
    if (!estimated_point_count) {
        return std::unexpected(std::move(estimated_point_count.error()));
    }
    auto doc_count = get_doc_count();
    if (!doc_count) {
        return std::unexpected(std::move(doc_count.error()));
    }
    auto points = size();
    if (!points) {
        return std::unexpected(std::move(points.error()));
    }
    const auto documents = static_cast<int64_t>(doc_count.value());
    const auto total = static_cast<int64_t>(points.value());

    if (estimated_point_count.value() >= total) {
        // All docs match.
        return documents;
    }
    if (total == documents || estimated_point_count.value() == 0) {
        return estimated_point_count.value();
    }

    // In case of multi values, estimate the number of docs with the urn problem
    // solution (see the reference implementation comment for the derivation), approximated for
    // points per doc << size().
    const double estimated = static_cast<double>(estimated_point_count.value());
    const double size_as_double = static_cast<double>(total);
    const double doc_as_double = static_cast<double>(documents);
    const auto doc_estimate = static_cast<int64_t>(
            doc_as_double * (1.0 - std::pow((size_as_double - estimated) / size_as_double, size_as_double / doc_as_double)));
    return doc_estimate == 0 ? 1 : doc_estimate;
}

} // namespace lucene
