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

module lucene_core:points_writer.impl;

import :points_writer;

import std;

import :bytes_ref;
import :error;
import :field_infos;
import :merge_state;
import :point_values;
import :points_reader;

namespace lucene {

namespace {

using DocMapPtr = std::shared_ptr<MergeStateDocMap>;
using Source = std::pair<std::shared_ptr<PointValues>, DocMapPtr>;
using Sources = std::vector<Source>;

/// Remaps the doc ids of one input segment into the merged segment and drops the
/// deleted ones.
///
/// Port of `IntersectVisitorImpl`.
class RemappingIntersectVisitor final : public IntersectVisitor {
public:
    RemappingIntersectVisitor(const DocMap &doc_map, IntersectVisitor &merged_visitor)
            : doc_map_(&doc_map), merged_visitor_(&merged_visitor) {}

    [[nodiscard]] VoidResult visit(int32_t /*doc_id*/) override {
        return err(LuceneError::illegal_state("visit must not be called without a packed value"));
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        auto new_doc_id = doc_map_->get(doc_id);
        if (!new_doc_id.has_value()) {
            return std::unexpected(std::move(new_doc_id.error()));
        }
        if (new_doc_id.value() == -1) {
            return {};
        }
        return merged_visitor_->visit_with_packed_value(new_doc_id.value(), packed_value);
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> /*min_packed_value*/,
                                           std::span<const uint8_t> /*max_packed_value*/) const override {
        return Relation::kCellCrossesQuery;
    }

private:
    const DocMap *doc_map_;
    IntersectVisitor *merged_visitor_;
};

/// A virtual point tree over the point trees of every input segment.
///
/// Port of `PointTreeImpl`: only `size` and `visit_doc_values` are meaningful,
/// because the points writer only ever walks the leaves of the tree it is given.
class MergingPointTree final : public PointTree {
public:
    MergingPointTree(size_t final_max_point_count, std::shared_ptr<const Sources> sources)
            : final_max_point_count_(final_max_point_count), sources_(std::move(sources)) {}

    [[nodiscard]] Result<bool> move_to_child() override { return false; }
    [[nodiscard]] Result<bool> move_to_sibling() override { return false; }
    [[nodiscard]] Result<bool> move_to_parent() override { return false; }

    [[nodiscard]] Result<BytesRef> get_min_packed_value() override {
        return err(LuceneError::unsupported_operation("the merging point tree has no bounds"));
    }
    [[nodiscard]] Result<BytesRef> get_max_packed_value() override {
        return err(LuceneError::unsupported_operation("the merging point tree has no bounds"));
    }

    [[nodiscard]] Result<size_t> size() override { return final_max_point_count_; }

    [[nodiscard]] VoidResult visit_doc_ids(IntersectVisitor & /*visitor*/) override {
        return err(LuceneError::unsupported_operation("visit_doc_ids is not supported while merging points"));
    }

    [[nodiscard]] VoidResult visit_doc_values(IntersectVisitor &visitor) override {
        for (const auto &source : *sources_) {
            auto tree = source.first->get_point_tree();
            if (!tree.has_value()) {
                return std::unexpected(std::move(tree.error()));
            }
            RemappingIntersectVisitor remapping(*source.second, visitor);
            if (auto visited = tree.value()->visit_doc_values(remapping); !visited.has_value()) {
                return visited;
            }
        }
        return {};
    }

private:
    size_t final_max_point_count_;
    std::shared_ptr<const Sources> sources_;
};

/// The point values of the merged segment, as far as the points writer cares.
///
/// Port of `PointValuesImpl`.
class MergingPointValues final : public PointValues {
public:
    MergingPointValues(size_t final_max_point_count, std::shared_ptr<const Sources> sources)
            : final_max_point_count_(final_max_point_count), sources_(std::move(sources)) {}

    [[nodiscard]] Result<std::optional<BytesRef>> get_min_packed_value() const override {
        return err(LuceneError::unsupported_operation("the merging point values have no bounds"));
    }
    [[nodiscard]] Result<std::optional<BytesRef>> get_max_packed_value() const override {
        return err(LuceneError::unsupported_operation("the merging point values have no bounds"));
    }
    [[nodiscard]] Result<size_t> get_num_dimensions() const override {
        return err(LuceneError::unsupported_operation("the merging point values have no dimensions"));
    }
    [[nodiscard]] Result<size_t> get_num_index_dimensions() const override {
        return err(LuceneError::unsupported_operation("the merging point values have no dimensions"));
    }
    [[nodiscard]] Result<size_t> get_bytes_per_dimension() const override {
        return err(LuceneError::unsupported_operation("the merging point values have no dimensions"));
    }
    [[nodiscard]] Result<size_t> size() const override { return final_max_point_count_; }
    [[nodiscard]] Result<int32_t> get_doc_count() const override {
        return err(LuceneError::unsupported_operation("the merging point values have no doc count"));
    }

    [[nodiscard]] Result<std::unique_ptr<PointTree>> get_point_tree() const override {
        return std::unique_ptr<PointTree>(new MergingPointTree(final_max_point_count_, sources_));
    }

private:
    size_t final_max_point_count_;
    std::shared_ptr<const Sources> sources_;
};

/// A [`PointsReader`] over the merged point values of one field.
///
/// Port of `PointsReaderImpl`.
class MergingPointsReader final : public PointsReader {
public:
    MergingPointsReader(const FieldInfo &field_info, size_t final_max_point_count, std::shared_ptr<const Sources> sources)
            : field_info_(&field_info), final_max_point_count_(final_max_point_count), sources_(std::move(sources)) {}

    [[nodiscard]] VoidResult check_integrity() override {
        return err(LuceneError::unsupported_operation("the merging points reader has no integrity check"));
    }

    [[nodiscard]] Result<std::shared_ptr<PointValues>> get_values(std::string_view field_name) const override {
        if (field_name != field_info_->name) {
            return err(LuceneError::illegal_argument("field name must match the field being merged"));
        }
        return std::shared_ptr<PointValues>(new MergingPointValues(final_max_point_count_, sources_));
    }

    [[nodiscard]] VoidResult close() override { return {}; }

private:
    const FieldInfo *field_info_;
    size_t final_max_point_count_;
    std::shared_ptr<const Sources> sources_;
};

} // namespace

VoidResult PointsWriter::merge(const MergeState &merge_state) {
    for (const auto &reader : merge_state.points_readers) {
        if (reader == nullptr) {
            continue;
        }
        if (auto checked = reader->check_integrity(); !checked.has_value()) {
            return checked;
        }
    }

    for (const auto &field_info : merge_state.merge_field_infos->fields()) {
        if (field_info.point_dimension_count != 0) {
            if (auto merged = merge_one_field(merge_state, field_info); !merged.has_value()) {
                return merged;
            }
        }
    }

    return finish();
}

VoidResult PointsWriter::merge_one_field(const MergeState &merge_state, const FieldInfo &field_info) {
    size_t max_point_count = 0;
    auto sources = std::make_shared<Sources>();

    for (size_t i = 0; i < merge_state.points_readers.size(); ++i) {
        const auto &points_reader = merge_state.points_readers[i];
        if (points_reader == nullptr) {
            continue;
        }
        const FieldInfo *reader_field_info = merge_state.field_infos->at(i)->by_name(field_info.name);
        if (reader_field_info == nullptr || reader_field_info->point_dimension_count == 0) {
            continue;
        }
        auto values = points_reader->get_values(field_info.name);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() == nullptr) {
            continue;
        }
        auto size = values.value()->size();
        if (!size.has_value()) {
            return std::unexpected(std::move(size.error()));
        }
        max_point_count += size.value();
        sources->emplace_back(std::move(values.value()), merge_state.doc_maps->at(i));
    }

    MergingPointsReader reader(field_info, max_point_count, sources);
    return write_field(field_info, reader);
}

} // namespace lucene
