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

export module lucene_core:reader_util;

import :types;
import std;

import :leaf_reader_context;

namespace lucene {

/// Utilities for walking reader trees.
///
/// Port of `ReaderUtil` (`core/index/reader_util.rs`).
///
/// **Deviation:** the reference implementation's `get_top_level_context` climbs the context tree; this
/// port stores the top-level metadata in every [`LeafReaderContext`], so the
/// lookup is a field access.
export class ReaderUtil {
public:
    /// The metadata of the top-level reader `leaf` belongs to.
    [[nodiscard]] static const TopParentMeta &get_top_level_context(const LeafReaderContext &leaf) {
        return leaf.top_parent();
    }

    /// The index of the reader that serves document `n`, given the `doc_base` of
    /// every reader.  Returns the last reader whose base is `<= n`.
    [[nodiscard]] static int32_t sub_index(int32_t n, std::span<const int32_t> doc_starts);

    /// The same as [`sub_index`], taking the leaves directly.
    [[nodiscard]] static int32_t sub_index_with_leaves(int32_t n, std::span<const LeafReaderContext> leaves);
};

} // namespace lucene
