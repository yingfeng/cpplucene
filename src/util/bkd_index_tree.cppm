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

export module lucene_core:bkd_index_tree;

import :types;
import :error;

import std;

import :bkd_config;
import :data_input;
import :index_input;

namespace lucene {

/// Header of a BKD meta file (`.kdm`, or the inline meta of an older layout).
///
/// Port of the meta parsing half of `BKDReader::init_with_meta`.
export struct BkdMeta {
    /// Validated tree parameters, rebuilt from the stored dimensions.
    BKDConfig config;
    /// Leaves in the tree.
    size_t num_leaves{0};
    /// Bounds of every indexed dimension (`packed_index_bytes_length` long each).
    std::vector<uint8_t> min_value;
    std::vector<uint8_t> max_value;
    /// Points stored in the tree.
    int64_t point_count{0};
    /// Distinct documents that contributed a point.
    int32_t doc_count{0};
    /// Length of the packed index.
    size_t index_len{0};
    /// File pointer of the first leaf block in the point values file.
    int64_t data_start_fp{0};
    /// File pointer of the packed index (in this file, or in the `.kdi`).
    int64_t index_fp{0};

    [[nodiscard]] static Result<BkdMeta> read(DataInput &meta_in);
};

/// One node of the parsed index.
export struct BkdIndexNode {
    /// Whether the node points at a leaf block instead of two children.
    bool leaf{false};
    /// Leaves below this node.
    size_t num_leaves{0};
    /// Leaf block pointer: the block itself for a leaf, the first block of the
    /// subtree for an inner node.
    size_t leaf_block_fp{0};
    /// Dimension this node splits on (inner nodes only).
    size_t dim{0};
    /// Split value of `dim`, `bytes_per_dim` long (inner nodes only).
    ///
    /// The packed index prefix-codes it against the split value last seen in the
    /// same dimension, so only the differing bytes are stored in the file.
    std::vector<uint8_t> split_value;
    /// Bounds of the subtree, `packed_index_bytes_length` long each.
    std::vector<uint8_t> min_value;
    std::vector<uint8_t> max_value;
    /// Children node indices (inner nodes only).
    size_t left{0};
    size_t right{0};
};

/// A parsed BKD index: the tree in pre-order, with `nodes[0]` as the root.
///
/// rlucene keeps the index on disk and walks it with a stack of cursors
/// (`BKDReader` inner node state).  Expanding it into nodes trades memory for a
/// much smaller traversal surface: every query then just walks a vector, and the
/// packed-index decoding lives in one place.
export struct BkdIndexTree {
    std::vector<BkdIndexNode> nodes;

    [[nodiscard]] size_t num_leaves() const noexcept {
        return nodes.empty() ? 0 : nodes.front().num_leaves;
    }
    [[nodiscard]] const BkdIndexNode &root() const noexcept { return nodes.front(); }

    /// Parses the packed index of `meta`, seeking to `meta.index_fp` first.
    ///
    /// Fails with `corrupt_index` when the decoded tree does not consume exactly
    /// `meta.index_len` bytes or when a stored subtree length disagrees with the
    /// bytes actually read.
    [[nodiscard]] static Result<BkdIndexTree> read(IndexInput &index_in, const BkdMeta &meta);
};

} // namespace lucene
