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

module lucene_core:bkd_config.impl;

import :bkd_config;

import :array_util;
import :bit_util;
import :error;
import :types;

import std;

namespace lucene {

Result<BKDConfig> BKDConfig::create(size_t num_dims, size_t num_index_dims, size_t bytes_per_dim,
                                    size_t max_points_in_leaf_node) {
    if (num_dims < 1 || num_dims > MAX_DIMS) {
        return err(LuceneError::illegal_argument("num_dims must be 1 .. " + std::to_string(MAX_DIMS) + " (got: " +
                                                 std::to_string(num_dims) + ")"));
    }
    if (num_index_dims < 1 || num_index_dims > MAX_INDEX_DIMS) {
        return err(LuceneError::illegal_argument("num_index_dims must be 1 .. " + std::to_string(MAX_INDEX_DIMS) +
                                                 " (got: " + std::to_string(num_index_dims) + ")"));
    }
    if (bytes_per_dim == 0) {
        return err(LuceneError::illegal_argument("bytes_per_dim must be > 0; got 0"));
    }
    if (num_index_dims > num_dims) {
        return err(LuceneError::illegal_argument("num_index_dims cannot exceed num_dims (got: " +
                                                 std::to_string(num_dims) + " vs " + std::to_string(num_index_dims) +
                                                 ")"));
    }
    if (max_points_in_leaf_node == 0) {
        return err(LuceneError::illegal_argument("max_points_in_leaf_node must be > 0; got 0"));
    }
    if (max_points_in_leaf_node > ArrayUtil::MAX_ARRAY_LENGTH) {
        return err(LuceneError::illegal_argument("max_points_in_leaf_node must be <= ArrayUtil::MAX_ARRAY_LENGTH (= " +
                                                 std::to_string(ArrayUtil::MAX_ARRAY_LENGTH) + "); got " +
                                                 std::to_string(max_points_in_leaf_node)));
    }

    BKDConfig config;
    config.num_dims = num_dims;
    config.num_index_dims = num_index_dims;
    config.bytes_per_dim = bytes_per_dim;
    config.max_points_in_leaf_node = max_points_in_leaf_node;
    return config;
}

std::string BKDConfig::to_string() const {
    return "BKDConfig(num_dims=" + std::to_string(num_dims) + ", num_index_dims=" + std::to_string(num_index_dims) +
           ", bytes_per_dim=" + std::to_string(bytes_per_dim) +
           ", max_points_in_leaf_node=" + std::to_string(max_points_in_leaf_node) + ")";
}

} // namespace lucene
