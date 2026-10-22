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

export module lucene_core:bkd_leaf_reader;

import :types;
import :error;

import std;

import :bkd_config;
import :index_input;

namespace lucene {

/// One decoded leaf block.
export struct BkdDecodedLeaf {
    /// Documents owning the points, in block order.
    std::vector<int32_t> doc_ids;
    /// Packed values (`packed_bytes_length()` each), one per point.
    std::vector<std::vector<uint8_t>> values;
    /// Per dimension common prefix that was stored.
    std::vector<size_t> common_prefix_lengths;
    /// Runs stored in the block.
    size_t cardinality{0};
    /// Whether the block was written with the "every value equal" encoding.
    bool all_equal{false};
    /// Dimension whose byte was run-length compressed, for the high-cardinality
    /// form (`std::nullopt` for the other two encodings).
    std::optional<size_t> compressed_dim;

    [[nodiscard]] size_t size() const noexcept { return doc_ids.size(); }
};

/// Decodes leaf blocks written by [`BkdLeafWriter`].
///
/// The leaf-reading half of the reference implementation's `BKDReader` (`read_doc_ids`,
/// `read_common_prefixes`, `visit_doc_values*`) pulled out as a standalone step:
/// the tree traversal only ever needs "give me the points stored in this block".
///
/// All three encodings the reference implementation produces are decoded: `0xFF` (all equal), `0xFE`
/// (one run per distinct value) and the high-cardinality form, whose marker byte
/// is the compressed dimension.
export struct BkdLeafReader {
    /// Seeks to `block_fp` in `data_in` and decodes the block.
    ///
    /// `data_in` is left positioned right after the block, so a caller walking
    /// blocks in file order can pass the same input again.
    [[nodiscard]] static Result<BkdDecodedLeaf> read_leaf_block(IndexInput &data_in, const BKDConfig &config,
                                                                size_t block_fp);
};

} // namespace lucene
