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

module lucene_core:sorting_points.impl;

import :sorting_points;

import std;

import :bytes_ref;
import :doc_id_set_iterator;
import :error;
import :index_sorter;
import :point_values;
import :points_reader;

namespace lucene {

namespace {

/// Forwards a point-tree visitor, renumbering the documents it is given.
class SortingIntersectVisitor final : public IntersectVisitor {
public:
    SortingIntersectVisitor(const IndexSortDocMap &doc_map, IntersectVisitor &visitor)
            : doc_map_(doc_map), visitor_(visitor) {}

    [[nodiscard]] VoidResult visit(int32_t doc_id) override {
        auto new_doc = doc_map_.old_to_new(doc_id);
        if (!new_doc) {
            return std::unexpected(std::move(new_doc.error()));
        }
        if (new_doc.value() < 0) {
            // The document was deleted before the sort.
            return {};
        }
        return visitor_.visit(new_doc.value());
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        auto new_doc = doc_map_.old_to_new(doc_id);
        if (!new_doc) {
            return std::unexpected(std::move(new_doc.error()));
        }
        if (new_doc.value() < 0) {
            return {};
        }
        return visitor_.visit_with_packed_value(new_doc.value(), packed_value);
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> min_packed_value,
                                           std::span<const uint8_t> max_packed_value) const override {
        return visitor_.compare(min_packed_value, max_packed_value);
    }

    [[nodiscard]] VoidResult grow(size_t count) override { return visitor_.grow(count); }

private:
    const IndexSortDocMap &doc_map_;
    IntersectVisitor &visitor_;
};

} // namespace

SortingPointTree::SortingPointTree(std::unique_ptr<PointTree> delegate, std::shared_ptr<const IndexSortDocMap> doc_map)
        : delegate_(std::move(delegate)), doc_map_(std::move(doc_map)) {}

std::unique_ptr<SortingPointTree> SortingPointTree::create(std::unique_ptr<PointTree> delegate,
                                                           std::shared_ptr<const IndexSortDocMap> doc_map) {
    return std::unique_ptr<SortingPointTree>(new SortingPointTree(std::move(delegate), std::move(doc_map)));
}

Result<bool> SortingPointTree::move_to_child() {
    return delegate_->move_to_child();
}

Result<bool> SortingPointTree::move_to_sibling() {
    return delegate_->move_to_sibling();
}

Result<bool> SortingPointTree::move_to_parent() {
    return delegate_->move_to_parent();
}

Result<BytesRef> SortingPointTree::get_min_packed_value() {
    return delegate_->get_min_packed_value();
}

Result<BytesRef> SortingPointTree::get_max_packed_value() {
    return delegate_->get_max_packed_value();
}

Result<size_t> SortingPointTree::size() {
    return delegate_->size();
}

VoidResult SortingPointTree::visit_doc_ids(IntersectVisitor &visitor) {
    SortingIntersectVisitor sorting_visitor(*doc_map_, visitor);
    return delegate_->visit_doc_ids(sorting_visitor);
}

VoidResult SortingPointTree::visit_doc_values(IntersectVisitor &visitor) {
    SortingIntersectVisitor sorting_visitor(*doc_map_, visitor);
    return delegate_->visit_doc_values(sorting_visitor);
}

std::unique_ptr<PointTree> SortingPointTree::try_clone() const {
    std::unique_ptr<PointTree> clone = delegate_->try_clone();
    if (clone == nullptr) {
        return nullptr;
    }
    return create(std::move(clone), doc_map_);
}

SortingPointValues::SortingPointValues(std::shared_ptr<PointValues> delegate,
                                       std::shared_ptr<const IndexSortDocMap> doc_map)
        : delegate_(std::move(delegate)), doc_map_(std::move(doc_map)) {}

std::unique_ptr<SortingPointValues> SortingPointValues::create(std::shared_ptr<PointValues> delegate,
                                                               std::shared_ptr<const IndexSortDocMap> doc_map) {
    return std::unique_ptr<SortingPointValues>(new SortingPointValues(std::move(delegate), std::move(doc_map)));
}

Result<std::optional<BytesRef>> SortingPointValues::get_min_packed_value() const {
    return delegate_->get_min_packed_value();
}

Result<std::optional<BytesRef>> SortingPointValues::get_max_packed_value() const {
    return delegate_->get_max_packed_value();
}

Result<size_t> SortingPointValues::get_num_dimensions() const {
    return delegate_->get_num_dimensions();
}

Result<size_t> SortingPointValues::get_num_index_dimensions() const {
    return delegate_->get_num_index_dimensions();
}

Result<size_t> SortingPointValues::get_bytes_per_dimension() const {
    return delegate_->get_bytes_per_dimension();
}

Result<size_t> SortingPointValues::size() const {
    return delegate_->size();
}

Result<int32_t> SortingPointValues::get_doc_count() const {
    return delegate_->get_doc_count();
}

Result<std::unique_ptr<PointTree>> SortingPointValues::get_point_tree() const {
    auto tree = delegate_->get_point_tree();
    if (!tree) {
        return std::unexpected(std::move(tree.error()));
    }
    if (tree.value() == nullptr) {
        return std::unique_ptr<PointTree>{};
    }
    return std::unique_ptr<PointTree>(SortingPointTree::create(std::move(tree.value()), doc_map_));
}

SortingPointsReader::SortingPointsReader(std::shared_ptr<PointsReader> delegate,
                                         std::shared_ptr<const IndexSortDocMap> doc_map)
        : delegate_(std::move(delegate)), doc_map_(std::move(doc_map)) {}

Result<std::unique_ptr<SortingPointsReader>> SortingPointsReader::create(
        std::shared_ptr<PointsReader> delegate, std::shared_ptr<const IndexSortDocMap> doc_map) {
    if (delegate == nullptr) {
        return err(LuceneError::illegal_argument("the points reader must not be null"));
    }
    if (doc_map == nullptr) {
        return err(LuceneError::illegal_argument("the doc map must not be null"));
    }
    return std::unique_ptr<SortingPointsReader>(new SortingPointsReader(std::move(delegate), std::move(doc_map)));
}

VoidResult SortingPointsReader::check_integrity() {
    return delegate_->check_integrity();
}

Result<std::shared_ptr<PointValues>> SortingPointsReader::get_values(std::string_view field_name) const {
    auto values = delegate_->get_values(field_name);
    if (!values) {
        return std::unexpected(std::move(values.error()));
    }
    if (values.value() == nullptr) {
        return std::shared_ptr<PointValues>{};
    }

    std::string key(field_name);
    auto cached = cache_.find(key);
    if (cached != cache_.end()) {
        return std::shared_ptr<PointValues>(cached->second);
    }
    std::shared_ptr<SortingPointValues> wrapped = SortingPointValues::create(values.value(), doc_map_);
    cache_.emplace(std::move(key), wrapped);
    return std::shared_ptr<PointValues>(std::move(wrapped));
}

bool SortingPointsReader::is_lucene90_points_reader() const {
    return delegate_->is_lucene90_points_reader();
}

VoidResult SortingPointsReader::close() {
    // Shared ownership: the reader that owns the wrapped reader closes it.
    return {};
}

} // namespace lucene
