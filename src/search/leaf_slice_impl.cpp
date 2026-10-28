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

module lucene_core:leaf_slice.impl;

import :leaf_slice;

import std;

import :error;
import :leaf_reader;
import :leaf_reader_context;
import :types;

namespace lucene {

namespace {

/// One leaf and its document count, for the biggest-first grouping.
struct SizedLeaf {
    int32_t ord{0};
    int32_t max_doc{0};
};

} // namespace

Result<std::vector<LeafSlice>> slices_with_limits(std::span<const LeafReaderContext> leaves, int32_t max_docs_per_slice,
                                                  size_t max_segments_per_slice) {
    if (max_docs_per_slice <= 0) {
        return err(LuceneError::illegal_argument("maxDocsPerSlice must be positive, got " +
                                                 std::to_string(max_docs_per_slice)));
    }
    if (max_segments_per_slice == 0) {
        return err(LuceneError::illegal_argument("maxSegmentsPerSlice must be positive"));
    }

    std::vector<SizedLeaf> sized;
    sized.reserve(leaves.size());
    for (const LeafReaderContext &leaf : leaves) {
        const LeafReader *reader = leaf.reader();
        if (reader == nullptr) {
            return err(LuceneError::illegal_state("a leaf context without a reader cannot be sliced"));
        }
        auto max_doc = reader->max_doc();
        if (!max_doc.has_value()) {
            return std::unexpected(std::move(max_doc.error()));
        }
        sized.push_back(SizedLeaf{leaf.ord(), max_doc.value()});
    }
    // Biggest leaves first, so that a huge segment does not end up in a slice
    // with several small ones; the ordinal breaks the ties, which keeps the
    // grouping deterministic.
    std::ranges::stable_sort(sized, [](const SizedLeaf &left, const SizedLeaf &right) {
        return left.max_doc > right.max_doc;
    });

    std::vector<LeafSlice> result;
    LeafSlice group;
    int64_t group_docs = 0;
    for (const SizedLeaf &leaf : sized) {
        group.leaves.push_back(leaf.ord);
        group_docs += leaf.max_doc;
        // A segment that is over the limit on its own still gets a slice of its
        // own: the port does not partition segments.
        if (group.leaves.size() >= max_segments_per_slice || group_docs > max_docs_per_slice) {
            result.push_back(std::move(group));
            group = LeafSlice{};
            group_docs = 0;
        }
    }
    if (!group.leaves.empty()) {
        result.push_back(std::move(group));
    }

    // Within a slice the leaves are visited in document order, which is the
    // ordinal order: the leaves of a reader are sorted by document base.
    for (LeafSlice &slice : result) {
        std::ranges::sort(slice.leaves);
    }
    return result;
}

Result<std::vector<LeafSlice>> slices(std::span<const LeafReaderContext> leaves) {
    return slices_with_limits(leaves, MAX_DOCS_PER_SLICE, MAX_SEGMENTS_PER_SLICE);
}

} // namespace lucene
