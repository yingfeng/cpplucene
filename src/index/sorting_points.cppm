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

export module lucene_core:sorting_points;

import :types;
import std;

import :bytes_ref;
import :doc_id_set_iterator;
import :error;
import :index_sorter;
import :point_values;
import :points_reader;

namespace lucene {

/// A KD-tree cursor that reports documents in index-sorted order.
///
/// Port of `SortingPointTree` (`core/index/sorting_codec_reader.rs`).
///
/// The tree still walks the pre-sort cells; only the documents it hands to the
/// visitor are renumbered.
///
/// **Deviation:** documents deleted before the sort (`old_to_new == -1`) are
/// skipped instead of being reported as document `-1`.
export class SortingPointTree final : public PointTree {
public:
    /// Wraps `delegate`; `doc_map` must outlive the wrapper (or be shared with
    /// it).
    [[nodiscard]] static std::unique_ptr<SortingPointTree> create(std::unique_ptr<PointTree> delegate,
                                                                 std::shared_ptr<const IndexSortDocMap> doc_map);

    [[nodiscard]] Result<bool> move_to_child() override;
    [[nodiscard]] Result<bool> move_to_sibling() override;
    [[nodiscard]] Result<bool> move_to_parent() override;
    [[nodiscard]] Result<BytesRef> get_min_packed_value() override;
    [[nodiscard]] Result<BytesRef> get_max_packed_value() override;
    [[nodiscard]] Result<size_t> size() override;
    [[nodiscard]] VoidResult visit_doc_ids(IntersectVisitor &visitor) override;
    [[nodiscard]] VoidResult visit_doc_values(IntersectVisitor &visitor) override;
    [[nodiscard]] std::unique_ptr<PointTree> try_clone() const override;

private:
    SortingPointTree(std::unique_ptr<PointTree> delegate, std::shared_ptr<const IndexSortDocMap> doc_map);

    std::unique_ptr<PointTree> delegate_;
    std::shared_ptr<const IndexSortDocMap> doc_map_;
};

/// The point values of a field, presented in index-sorted order.
///
/// Port of `SortingPointValues` (`core/index/sorting_codec_reader.rs`).
export class SortingPointValues final : public PointValues {
public:
    /// Wraps `delegate`; `doc_map` must outlive the wrapper.
    [[nodiscard]] static std::unique_ptr<SortingPointValues> create(std::shared_ptr<PointValues> delegate,
                                                                   std::shared_ptr<const IndexSortDocMap> doc_map);

    [[nodiscard]] Result<std::optional<BytesRef>> get_min_packed_value() const override;
    [[nodiscard]] Result<std::optional<BytesRef>> get_max_packed_value() const override;
    [[nodiscard]] Result<size_t> get_num_dimensions() const override;
    [[nodiscard]] Result<size_t> get_num_index_dimensions() const override;
    [[nodiscard]] Result<size_t> get_bytes_per_dimension() const override;
    [[nodiscard]] Result<size_t> size() const override;
    [[nodiscard]] Result<int32_t> get_doc_count() const override;
    [[nodiscard]] Result<std::unique_ptr<PointTree>> get_point_tree() const override;

private:
    SortingPointValues(std::shared_ptr<PointValues> delegate, std::shared_ptr<const IndexSortDocMap> doc_map);

    std::shared_ptr<PointValues> delegate_;
    std::shared_ptr<const IndexSortDocMap> doc_map_;
};

/// The point values of a segment, presented in index-sorted order.
///
/// Port of `PointsReaderImpl` (`core/index/sorting_codec_reader.rs`).
///
/// **Deviation:** [`close`] does nothing: the wrapped reader is shared through a
/// `std::shared_ptr`, so its owner closes it.
export class SortingPointsReader final : public PointsReader {
public:
    /// Wraps `delegate`; `doc_map` must outlive the wrapper.
    [[nodiscard]] static Result<std::unique_ptr<SortingPointsReader>> create(
            std::shared_ptr<PointsReader> delegate, std::shared_ptr<const IndexSortDocMap> doc_map);

    [[nodiscard]] VoidResult check_integrity() override;
    [[nodiscard]] Result<std::shared_ptr<PointValues>> get_values(std::string_view field_name) const override;
    [[nodiscard]] bool is_lucene90_points_reader() const override;
    [[nodiscard]] VoidResult close() override;

private:
    SortingPointsReader(std::shared_ptr<PointsReader> delegate, std::shared_ptr<const IndexSortDocMap> doc_map);

    std::shared_ptr<PointsReader> delegate_;
    std::shared_ptr<const IndexSortDocMap> doc_map_;
    /// One wrapper per field, so repeated `get_values` calls share it and the
    /// pointers it hands out stay valid.
    mutable std::map<std::string, std::shared_ptr<SortingPointValues>, std::less<>> cache_;
};

} // namespace lucene
