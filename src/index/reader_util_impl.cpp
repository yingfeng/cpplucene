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

module lucene_core:reader_util.impl;

import :reader_util;

import :leaf_reader_context;
import :types;

import std;

namespace lucene {

int32_t ReaderUtil::sub_index(int32_t n, std::span<const int32_t> doc_starts) {
    const int32_t size = static_cast<int32_t>(doc_starts.size());
    int32_t lo = 0;           // search starts array
    int32_t hi = size - 1;    // for first element less than n, return its index

    while (hi >= lo) {
        const int32_t mid = (lo + hi) >> 1;
        const int32_t mid_value = doc_starts[static_cast<size_t>(mid)];
        if (n < mid_value) {
            hi = mid - 1;
        } else if (n > mid_value) {
            lo = mid + 1;
        } else {
            // Found a match: scan to the last reader with this base.
            int32_t last = mid;
            while (last + 1 < size && doc_starts[static_cast<size_t>(last + 1)] == mid_value) {
                ++last;
            }
            return last;
        }
    }
    return hi;
}

int32_t ReaderUtil::sub_index_with_leaves(int32_t n, std::span<const LeafReaderContext> leaves) {
    const int32_t size = static_cast<int32_t>(leaves.size());
    int32_t lo = 0;
    int32_t hi = size - 1;

    while (hi >= lo) {
        const int32_t mid = (lo + hi) >> 1;
        const int32_t mid_value = leaves[static_cast<size_t>(mid)].doc_base();
        if (n < mid_value) {
            hi = mid - 1;
        } else if (n > mid_value) {
            lo = mid + 1;
        } else {
            int32_t last = mid;
            while (last + 1 < size && leaves[static_cast<size_t>(last + 1)].doc_base() == mid_value) {
                ++last;
            }
            return last;
        }
    }
    return hi > 0 ? hi : 0;
}

} // namespace lucene
