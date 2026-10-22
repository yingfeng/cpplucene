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

export module lucene_core:bkd_config;

import :types;
import :error;
import :bit_util;

import std;

namespace lucene {

/// Codec name written into the BKD meta header (the reference implementation's `CODEC_NAME`).
export inline constexpr std::string_view BKD_CODEC_NAME = "BKD";
/// Meta-file version this port reads and writes (Lucene 9's `VERSION_META_FILE`).
export inline constexpr int32_t BKD_VERSION_CURRENT = 9;

/// Basic parameters for indexing points in a BKD tree.
///
/// Port of `BKDConfig` (`util/bkd/bkd_config.rs`).
export struct BKDConfig {
    /// Default maximum number of points in a leaf block.
    static constexpr size_t DEFAULT_MAX_POINTS_IN_LEAF_NODE = 512;
    /// Maximum number of dimensions (2 * `MAX_INDEX_DIMS`).
    static constexpr size_t MAX_DIMS = 16;
    /// Maximum number of indexed dimensions.
    static constexpr size_t MAX_INDEX_DIMS = 8;

    /// How many dimensions are stored at a leaf (data) node.
    size_t num_dims{0};
    /// How many dimensions are indexed in the internal nodes.
    size_t num_index_dims{0};
    /// How many bytes each value in each dimension takes.
    size_t bytes_per_dim{0};
    /// Maximum number of points allowed in a leaf block.
    size_t max_points_in_leaf_node{0};

    /// Validates the parameters and builds a config.
    ///
    /// Rejects a dimension count outside `[1, MAX_DIMS]`, an index dimension
    /// count outside `[1, MAX_INDEX_DIMS]`, more indexed than total dimensions,
    /// a zero `bytes_per_dim`, and a leaf size of zero or above
    /// `ArrayUtil::MAX_ARRAY_LENGTH`.
    [[nodiscard]] static Result<BKDConfig> create(size_t num_dims, size_t num_index_dims, size_t bytes_per_dim,
                                                  size_t max_points_in_leaf_node);

    /// `num_dims * bytes_per_dim`.
    [[nodiscard]] size_t packed_bytes_length() const noexcept { return num_dims * bytes_per_dim; }

    /// `num_index_dims * bytes_per_dim`.
    [[nodiscard]] size_t packed_index_bytes_length() const noexcept { return num_index_dims * bytes_per_dim; }

    /// `packed_bytes_length() + sizeof(int32_t)` (the packed point plus its
    /// document id).
    [[nodiscard]] size_t bytes_per_doc() const noexcept { return packed_bytes_length() + BitUtil::INT_BYTES; }

    [[nodiscard]] std::string to_string() const;
};

} // namespace lucene
