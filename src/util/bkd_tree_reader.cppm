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

export module lucene_core:bkd_tree_reader;

import :types;
import :error;

import std;

import :bkd_config;
import :bkd_index_tree;
import :index_input;

namespace lucene {

/// Callback for one point: the document that owns it plus its packed value.
///
/// A document with several points is reported once per point; callers that need
/// distinct documents deduplicate themselves (this is what Lucene's
/// `IntersectVisitor` leaves to the collector as well).
export using BkdPointVisitor = std::function<VoidResult(int32_t doc_id, std::span<const uint8_t> packed_value)>;

/// Walks a [`BkdIndexTree`] and reports the points that match a bounding box.
///
/// Port of the traversal half of the reference implementation's `BKDReader`
/// (`add_all`/`visit_doc_values`/`read_leaf_block`): the index is walked with the
/// pre-expanded tree instead of a cursor stack, and the leaves are decoded by
/// [`BkdLeafReader`].
export struct BkdTreeReader {
    /// Visits every point inside the box `[min, max)` per indexed dimension.
    ///
    /// `min` and `max` are `packed_index_bytes_length()` long — the data-only
    /// dimensions are not filtered on, exactly like Lucene, where a cell is a box
    /// over the indexed dimensions only.
    [[nodiscard]] static VoidResult intersect(const BkdIndexTree &tree, const BkdMeta &meta, IndexInput &data_in,
                                              std::span<const uint8_t> min, std::span<const uint8_t> max,
                                              const BkdPointVisitor &visitor);

    /// Visits every point in the tree, in leaf order.
    [[nodiscard]] static VoidResult visit_all(const BkdIndexTree &tree, const BkdMeta &meta, IndexInput &data_in,
                                              const BkdPointVisitor &visitor);
};

} // namespace lucene
