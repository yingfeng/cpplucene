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

export module lucene_core:multi_sorter;

import :types;
import std;

import :codec_reader;
import :error;
import :merge_state;
import :sort_field;

namespace lucene {

/// Merge-sorts the documents of several already index-sorted segments.
///
/// Port of `MultiSorter` (`core/index/multi_sorter.rs`).
///
/// Each incoming segment is already sorted by the same [`Sort`]; this walks them
/// with a priority queue and produces one [`MergeStateDocMap`] per segment that
/// maps its documents into the merged, still sorted segment.
///
/// **Deviation:** the reference implementation also maps documents through the
/// parent field of a block index (`SegmentMetadata::has_blocks`); the port has no
/// block support, so a segment is always treated as flat.
export struct MultiSorter {
    /// The doc maps for `readers`, or `nullopt` when the segments are already in
    /// order (nothing has to be rewritten).
    [[nodiscard]] static Result<std::optional<std::vector<std::shared_ptr<MergeStateDocMap>>>> sort(
            const Sort &sort, const std::vector<std::shared_ptr<CodecReader>> &readers);
};

} // namespace lucene
