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

export module lucene_core:point_values;

import :types;
import std;

import :bytes_ref;
import :doc_id_set_iterator;
import :error;
import :ints_ref;

namespace lucene {

/// Used by [`PointValues::intersect`] to check how each recursive cell
/// corresponds to the query.
///
/// The ordinals match the reference implementation's `Relation::ordinal`, and are what the BKD
/// `IntersectState` stores.
export enum class Relation : uint8_t {
    /// Return this if the query fully contains the cell.
    kCellInsideQuery = 0,
    /// Return this if the cell and query do not overlap.
    kCellOutsideQuery = 1,
    /// Return this if the cell partially overlaps the query.
    kCellCrossesQuery = 2,
};

/// The ordinal of `relation`, as the reference implementation's `Relation::ordinal` reports it.
[[nodiscard]] constexpr int32_t relation_ordinal(Relation relation) noexcept {
    return static_cast<int32_t>(relation);
}

/// We recurse the [`PointTree`], using a provided instance of this to guide the
/// recursion.
///
/// Port of the `IntersectVisitor` trait (`core/index/point_values.rs`).
export class IntersectVisitor {
public:
    virtual ~IntersectVisitor() = default;

    /// Called for all documents in a leaf cell that is fully contained by the
    /// query.  The consumer should blindly accept the document id.
    [[nodiscard]] virtual VoidResult visit(int32_t doc_id) = 0;

    /// Like [`visit`], but for a whole iterator; implementations may have
    /// optimizations.
    [[nodiscard]] virtual VoidResult visit_with_iterator(DocIdSetIterator &iterator) {
        while (true) {
            auto doc_id = iterator.next_doc();
            if (!doc_id) {
                return std::unexpected(std::move(doc_id.error()));
            }
            if (doc_id.value() == NO_MORE_DOCS) {
                return {};
            }
            if (auto visited = visit(doc_id.value()); !visited) {
                return visited;
            }
        }
    }

    /// Like [`visit`], but for a run of document ids.
    [[nodiscard]] virtual VoidResult visit_with_ints_ref(const IntsRef &ints_ref) {
        for (const int32_t doc_id : ints_ref.as_slice()) {
            if (auto visited = visit(doc_id); !visited) {
                return visited;
            }
        }
        return {};
    }

    /// Called for each document and point in a cell that crosses the query
    /// boundary.  The current point is required to match the query.
    [[nodiscard]] virtual VoidResult visit_with_packed_value(int32_t doc_id,
                                                             std::span<const uint8_t> packed_value) = 0;

    /// Like [`visit_with_packed_value`], but for a whole iterator; the same
    /// packed value applies to every document it yields.
    [[nodiscard]] virtual VoidResult visit_iterator_with_packed_value(DocIdSetIterator &iterator,
                                                                     std::span<const uint8_t> packed_value) {
        while (true) {
            auto doc_id = iterator.next_doc();
            if (!doc_id) {
                return std::unexpected(std::move(doc_id.error()));
            }
            if (doc_id.value() == NO_MORE_DOCS) {
                return {};
            }
            if (auto visited = visit_with_packed_value(doc_id.value(), packed_value); !visited) {
                return visited;
            }
        }
    }

    /// Called for each cell and returns the relation of the cell to the query.
    [[nodiscard]] virtual Result<Relation> compare(std::span<const uint8_t> min_packed_value,
                                                   std::span<const uint8_t> max_packed_value) const = 0;

    /// Notifies the visitor that a new segment of data is about to start, with
    /// `count` documents.
    [[nodiscard]] virtual VoidResult grow(size_t /*count*/) { return {}; }
};

/// Basic operations to read the KD-tree.
///
/// Port of the `PointTree` trait (`core/index/point_values.rs`).
///
/// **Deviation:** the reference implementation's `TryClone` becomes [`try_clone`]; the `Cow` return
/// values become owned [`BytesRef`], which is what `Cow::Owned` held anyway.
export class PointTree {
public:
    virtual ~PointTree() = default;

    /// Moves to the first child node and returns `true` upon success; `false`
    /// for leaf nodes.
    [[nodiscard]] virtual Result<bool> move_to_child() {
        return err(LuceneError::need_implemented("move_to_child not implemented"));
    }

    /// Moves to the next sibling node and returns `true` upon success; `false`
    /// when the current node has no more siblings.
    [[nodiscard]] virtual Result<bool> move_to_sibling() {
        return err(LuceneError::need_implemented("move_to_sibling not implemented"));
    }

    /// Moves to the parent node and returns `true` upon success; `false` for
    /// the root node.
    [[nodiscard]] virtual Result<bool> move_to_parent() {
        return err(LuceneError::need_implemented("move_to_parent not implemented"));
    }

    /// The minimum packed value of the current node.
    [[nodiscard]] virtual Result<BytesRef> get_min_packed_value() {
        return err(LuceneError::need_implemented("get_min_packed_value not implemented"));
    }

    /// The maximum packed value of the current node.
    [[nodiscard]] virtual Result<BytesRef> get_max_packed_value() {
        return err(LuceneError::need_implemented("get_max_packed_value not implemented"));
    }

    /// The number of points below the current node.
    [[nodiscard]] virtual Result<size_t> size() {
        return err(LuceneError::need_implemented("size not implemented"));
    }

    /// Visits all the documents below the current node.
    [[nodiscard]] virtual VoidResult visit_doc_ids(IntersectVisitor & /*visitor*/) {
        return err(LuceneError::need_implemented("visit_doc_ids not implemented"));
    }

    /// Visits all the documents and values below the current node.
    [[nodiscard]] virtual VoidResult visit_doc_values(IntersectVisitor & /*visitor*/) {
        return err(LuceneError::need_implemented("visit_doc_values not implemented"));
    }

    /// A fresh, independent cursor over the same tree.
    [[nodiscard]] virtual std::unique_ptr<PointTree> try_clone() const {
        return nullptr;
    }
};

/// Recurses `point_tree`, guided by `visitor`.
///
/// Port of `intersect_with_point_tree` (`core/index/point_values.rs`).
[[nodiscard]] VoidResult intersect_with_point_tree(IntersectVisitor &visitor, PointTree &point_tree);

/// Estimates how many points [`intersect_with_point_tree`] would visit, without
/// visiting them.
///
/// Port of `estimate_point_count_with_point_tree`
/// (`core/index/point_values.rs`).
[[nodiscard]] Result<int64_t> estimate_point_count_with_point_tree(const IntersectVisitor &visitor,
                                                                   PointTree &point_tree, int64_t upper_bound);

/// Access to indexed points of a field.
///
/// Port of the `PointValues` trait (`core/index/point_values.rs`).
///
/// **Deviations:**
///  * the associated `PointTree`/`MutablePointTree` types and
///    `PointTreeEnum` collapse into a single `std::unique_ptr<PointTree>`
///    return: the KD-tree readers always know which cursor they hand out;
///  * the reader-wide helpers (`size`, `min`, `max` over a whole
///    `IndexReader`) are not ported yet, because there is no `IndexReader` in
///    this port so far.
export class PointValues {
public:
    virtual ~PointValues() = default;

    /// Minimum value for each dimension, packed, or `nullopt` when
    /// [`size`] is `0`.
    [[nodiscard]] virtual Result<std::optional<BytesRef>> get_min_packed_value() const = 0;

    /// Maximum value for each dimension, packed, or `nullopt` when
    /// [`size`] is `0`.
    [[nodiscard]] virtual Result<std::optional<BytesRef>> get_max_packed_value() const = 0;

    /// How many dimensions are represented in the values.
    [[nodiscard]] virtual Result<size_t> get_num_dimensions() const = 0;

    /// How many dimensions are used for the index.
    [[nodiscard]] virtual Result<size_t> get_num_index_dimensions() const = 0;

    /// The number of bytes per dimension.
    [[nodiscard]] virtual Result<size_t> get_bytes_per_dimension() const = 0;

    /// The total number of indexed points across all documents.
    [[nodiscard]] virtual Result<size_t> size() const = 0;

    /// The total number of documents that have indexed at least one point.
    [[nodiscard]] virtual Result<int32_t> get_doc_count() const = 0;

    /// A new cursor over the tree.
    [[nodiscard]] virtual Result<std::unique_ptr<PointTree>> get_point_tree() const = 0;

    /// Finds all documents and points matching `visitor`.
    ///
    /// This method does not enforce live documents, so it is up to the caller to
    /// test whether each document is deleted, if necessary.
    [[nodiscard]] virtual VoidResult intersect(IntersectVisitor &visitor) const;

    /// Estimates the number of points [`intersect`] would visit, which should
    /// run many times faster than the intersection itself.
    [[nodiscard]] virtual Result<int64_t> estimate_point_count(const IntersectVisitor &visitor) const;

    /// Estimates the number of documents [`intersect`] would match.
    [[nodiscard]] virtual Result<int64_t> estimate_doc_count(const IntersectVisitor &visitor) const;
};

} // namespace lucene
