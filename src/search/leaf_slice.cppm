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

export module lucene_core:leaf_slice;

import :types;
import std;

import :error;
import :leaf_reader_context;

namespace lucene {

/// The leaves one search task visits.
///
/// Port of `LeafSlice` (`core/search/index_searcher.rs`).
///
/// **Deviation:** the reference implementation can also split one huge segment
/// into several partitions so that its documents are searched in parallel; the
/// port searches whole segments only, so a slice is a list of leaf ordinals.
/// Either way a leaf belongs to exactly one slice, which is what lets the
/// collectors of the slices run concurrently: no two tasks touch the same leaf
/// reader.
export struct LeafSlice {
    /// The ordinals of the leaves of this slice, in document order.
    std::vector<int32_t> leaves;
};

/// How many documents a slice aims to hold.
export inline constexpr int32_t MAX_DOCS_PER_SLICE = 250'000;

/// How many segments a slice holds at most.
export inline constexpr size_t MAX_SEGMENTS_PER_SLICE = 5;

/// Groups `leaves` into slices of at most `max_segments_per_slice` segments and
/// about `max_docs_per_slice` documents, biggest leaves first.
///
/// Port of `do_slices` (`core/search/index_searcher.rs`) with
/// `allow_segment_partitions = false`: a slice either holds whole segments, or a
/// single segment that does not fit in one slice by itself.
[[nodiscard]] Result<std::vector<LeafSlice>> slices_with_limits(std::span<const LeafReaderContext> leaves,
                                                                int32_t max_docs_per_slice,
                                                                size_t max_segments_per_slice);

/// [`slices_with_limits`] with [`MAX_DOCS_PER_SLICE`] and
/// [`MAX_SEGMENTS_PER_SLICE`].
[[nodiscard]] Result<std::vector<LeafSlice>> slices(std::span<const LeafReaderContext> leaves);

} // namespace lucene
