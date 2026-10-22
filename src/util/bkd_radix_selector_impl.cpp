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

module lucene_core:bkd_radix_selector.impl;

import :bkd_radix_selector;

import :bkd_heap_point_writer;
import :bkd_util;
import :debug;
import :error;
import :types;

import std;

namespace lucene {

std::array<size_t, 257> BkdRadixSelector::radix_partition(size_t from, size_t to, size_t byte_index) {
    std::array<size_t, 257> boundaries{};
    const size_t count = to - from;

    // Histogram first: bucket `b` counts into `boundaries[b + 1]`, so the prefix
    // sum below turns the array into "first index of bucket b + 1".
    for (size_t i = from; i < to; ++i) {
        const size_t bucket = writer_->packed_value(i).packed_value()[byte_index];
        ++boundaries[bucket + 1];
    }
    for (size_t bucket = 0; bucket < 256; ++bucket) {
        boundaries[bucket + 1] += boundaries[bucket];
    }
    debug_check(boundaries[256] == count, "the histogram must cover the whole slice");

    // American flag permutation: walk each bucket's target range and swap every
    // misplaced point into the range of its own bucket.  Each swap settles
    // exactly one point, so this is O(n) swaps.
    std::array<size_t, 256> next{};
    for (size_t bucket = 0; bucket < 256; ++bucket) {
        next[bucket] = from + boundaries[bucket];
    }
    for (size_t bucket = 0; bucket < 256; ++bucket) {
        const size_t bucket_end = from + boundaries[bucket + 1];
        while (next[bucket] < bucket_end) {
            const size_t index = next[bucket];
            const size_t actual = writer_->packed_value(index).packed_value()[byte_index];
            if (actual == bucket) {
                ++next[bucket];
                continue;
            }
            swap_points(index, next[actual]);
            ++next[actual];
        }
    }

    // Hand back absolute boundaries.
    for (size_t bucket = 0; bucket < 257; ++bucket) {
        boundaries[bucket] += from;
    }
    return boundaries;
}

std::vector<uint8_t> BkdRadixSelector::value_at(size_t index, size_t partitioned_through) {
    const auto bytes = writer_->config().bytes_per_dim;
    std::vector<uint8_t> value(bytes, 0);
    const auto packed = writer_->packed_value(index).packed_value();
    // Bytes before the skipped prefix are equal across the block by definition,
    // and the bytes past the last radix level are free: zeroing them turns the
    // partition point's value into the smallest value that still separates the
    // two sides.  The packed index needs that fixed anchor, because whatever low
    // bytes the point at the boundary happens to carry make the split value jump
    // around as the tree is walked.
    const size_t keep = partitioned_through <= dim_ + prefix_ ? bytes : partitioned_through - dim_;
    std::copy_n(packed.begin() + static_cast<std::ptrdiff_t>(dim_ + prefix_),
                static_cast<std::ptrdiff_t>(keep - prefix_),
                value.begin() + static_cast<std::ptrdiff_t>(prefix_));
    return value;
}

Result<std::vector<uint8_t>> BkdRadixSelector::select(size_t from, size_t to, size_t partition_point, size_t dim,
                                                      size_t dim_common_prefix) {
    const auto &config = writer_->config();
    if (partition_point < from || partition_point >= to) {
        return err(LuceneError::illegal_argument("partitionPoint must be in [from, to) (got " +
                                                 std::to_string(partition_point) + " for [" + std::to_string(from) +
                                                 ", " + std::to_string(to) + "))"));
    }
    if (dim + config.bytes_per_dim > config.packed_bytes_length()) {
        return err(LuceneError::illegal_argument("dimension offset is outside the packed point"));
    }
    if (dim_common_prefix >= config.bytes_per_dim) {
        return err(LuceneError::illegal_argument("dim_common_prefix must be smaller than bytes_per_dim"));
    }

    dim_ = dim;
    prefix_ = dim_common_prefix;
    compare_length_ = config.bytes_per_dim - dim_common_prefix;

    // Descend through the bucket that contains `partition_point`.  Every level
    // splits the slice on one byte, so points outside the descending bucket are
    // already ordered against everything inside it.
    size_t start = from;
    size_t end = to;
    size_t byte = dim_ + prefix_;
    while (byte < dim_ + config.bytes_per_dim && end - start > 1) {
        const auto boundaries = radix_partition(start, end, byte);
        size_t bucket = 0;
        while (boundaries[bucket + 1] <= partition_point) {
            ++bucket;
        }
        start = boundaries[bucket];
        end = boundaries[bucket + 1];
        ++byte;
    }

    return value_at(partition_point, byte);
}

void BkdRadixSelector::radix_sort(size_t start, size_t end, size_t byte) {
    if (end - start < 2 || byte >= dim_ + writer_->config().bytes_per_dim) {
        return;
    }
    const auto boundaries = radix_partition(start, end, byte);
    for (size_t bucket = 0; bucket < 256; ++bucket) {
        radix_sort(boundaries[bucket], boundaries[bucket + 1], byte + 1);
    }
}

VoidResult BkdRadixSelector::sort(size_t from, size_t to, size_t dim, size_t dim_common_prefix) {
    const auto &config = writer_->config();
    if (from > to || to > writer_->count()) {
        return err(LuceneError::illegal_argument("the sorted slice must be inside the written points"));
    }
    if (dim + config.bytes_per_dim > config.packed_bytes_length()) {
        return err(LuceneError::illegal_argument("dimension offset is outside the packed point"));
    }
    if (dim_common_prefix >= config.bytes_per_dim) {
        return err(LuceneError::illegal_argument("dim_common_prefix must be smaller than bytes_per_dim"));
    }

    dim_ = dim;
    prefix_ = dim_common_prefix;
    compare_length_ = config.bytes_per_dim - dim_common_prefix;

    if (to - from < 2) {
        return {};
    }
    radix_sort(from, to, dim_ + prefix_);
    return {};
}

} // namespace lucene
