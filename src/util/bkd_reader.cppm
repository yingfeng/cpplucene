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

export module lucene_core:bkd_reader;

import :types;
import :error;

import std;

import :bkd_config;
import :bkd_index_tree;
import :bytes_ref;
import :index_input;
import :point_values;

namespace lucene {

/// One cursor over a [`BkdReader`]'s index: a [`PointTree`] positioned at some
/// node of the pre-expanded [`BkdIndexTree`].
///
/// Port of rlucene's `BKDPointTree` (`util/bkd/bkd_reader.rs`), minus the cursor
/// stacks: the packed index is decoded once into [`BkdIndexTree`], so every
/// position is just a node index and the cell bounds of a node are its stored
/// subtree bounds.  The leaf blocks themselves are still read on demand, through
/// [`BkdLeafReader`].
export class BkdPointTree final : public PointTree {
public:
    /// A cursor over `reader`'s tree, positioned at the root.
    [[nodiscard]] static Result<std::unique_ptr<BkdPointTree>> create(std::shared_ptr<const BkdMeta> meta,
                                                                     std::shared_ptr<const BkdIndexTree> index,
                                                                     std::unique_ptr<IndexInput> data_in);

    /// Forgets every position and starts over at the root.
    void reset() noexcept { node_index_ = 0; }

    /// Index of the node this cursor sits on (0 is the root).
    [[nodiscard]] size_t node_index() const noexcept { return node_index_; }

    /// `true` when the cursor sits on a leaf block.
    [[nodiscard]] bool is_leaf() const noexcept;

    // ---- PointTree ----
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
    BkdPointTree(std::shared_ptr<const BkdMeta> meta, std::shared_ptr<const BkdIndexTree> index,
                 std::unique_ptr<IndexInput> data_in);

    [[nodiscard]] const BkdIndexNode &node() const noexcept { return index_->nodes[node_index_]; }

    /// Leaves of the whole tree that precede this node's first leaf.
    [[nodiscard]] size_t leaves_before() const;

    /// Feeds every point below the current node to `visitor`, decoding the leaf
    /// blocks as it goes.  `with_values == false` reports document ids only
    /// (the "cell fully inside the query" path).
    [[nodiscard]] VoidResult visit_subtree(IntersectVisitor &visitor, bool with_values);

    std::shared_ptr<const BkdMeta> meta_;
    std::shared_ptr<const BkdIndexTree> index_;
    std::unique_ptr<IndexInput> data_in_;
    size_t node_index_{0};
};

/// Reads a BKD tree that was written by [`BkdWriter`].
///
/// Port of `BKDReader` (`util/bkd/bkd_reader.rs`), which is both the
/// [`PointValues`] of one point field and the factory for its [`PointTree`]
/// cursors.  rlucene keeps the packed index on disk and walks it with a stack of
/// cursors; this port decodes it up front into a [`BkdIndexTree`], so a cursor
/// is a node index and [`BkdTreeReader`] stays a plain traversal helper.
///
/// Every cursor owns its own clone of the data input, so cursors never fight
/// over a seek position and one reader can serve concurrent queries.
export class BkdReader final : public PointValues {
public:
    /// Wraps an already parsed meta block and index.
    ///
    /// `data_in` is the point values file positioned anywhere; the reader clones
    /// it for each cursor.
    [[nodiscard]] static Result<std::shared_ptr<BkdReader>> open(std::shared_ptr<const BkdMeta> meta,
                                                                std::shared_ptr<const BkdIndexTree> index,
                                                                std::unique_ptr<IndexInput> data_in);

    // ---- PointValues ----
    [[nodiscard]] Result<std::optional<BytesRef>> get_min_packed_value() const override;
    [[nodiscard]] Result<std::optional<BytesRef>> get_max_packed_value() const override;
    [[nodiscard]] Result<size_t> get_num_dimensions() const override;
    [[nodiscard]] Result<size_t> get_num_index_dimensions() const override;
    [[nodiscard]] Result<size_t> get_bytes_per_dimension() const override;
    [[nodiscard]] Result<size_t> size() const override;
    [[nodiscard]] Result<int32_t> get_doc_count() const override;
    [[nodiscard]] Result<std::unique_ptr<PointTree>> get_point_tree() const override;

    // ---- accessors used by the codec and by tests ----
    [[nodiscard]] const BkdMeta &meta() const noexcept { return *meta_; }
    [[nodiscard]] const BKDConfig &config() const noexcept { return meta_->config; }
    [[nodiscard]] const BkdIndexTree &index() const noexcept { return *index_; }
    /// The shared data input; callers that need to read blocks clone it first.
    [[nodiscard]] IndexInput &data_input() const noexcept { return *data_in_; }

private:
    BkdReader(std::shared_ptr<const BkdMeta> meta, std::shared_ptr<const BkdIndexTree> index,
              std::unique_ptr<IndexInput> data_in);

    std::shared_ptr<const BkdMeta> meta_;
    std::shared_ptr<const BkdIndexTree> index_;
    std::unique_ptr<IndexInput> data_in_;
};

} // namespace lucene
