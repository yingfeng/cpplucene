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

export module lucene_core:bkd_radix_selector;

import :types;
import :error;

import std;

import :bkd_heap_point_writer;

namespace lucene {

/// Partitions and sorts in-heap points by one dimension.
///
/// Port of `BKDRadixSelector` (`util/bkd/bkd_radix_selector.rs`).
///
/// Both operations are MSD radix passes over the bytes of one dimension: each
/// level counts the 256 possible byte values of the slice and permutes the
/// points in place so that bucket `b` occupies its own contiguous range, then
/// descends into the single bucket that can still contain the answer.  One
/// level is `O(n)` with at most `n` swaps, so a partition costs `O(bytesPerDim)`
/// passes instead of the `O(n log n)` comparisons quicksort needs — this is the
/// 256-way histogram optimisation rlucene uses.
///
///  * `select(from, to, partition_point, dim)` guarantees that every point in
///    `[from, partition_point)` is `<=` the returned value on `dim`, and every
///    point in `[partition_point, to)` is `>=` it;
///  * `sort(from, to, dim)` orders the slice by `dim`.
///
/// **Deviation:** rlucene also partitions the offline (spilled) point writer.
/// This port partitions the heap only; [`BkdWriter`] materialises each spilled
/// leaf before encoding it, so the selector never sees an offline slice.
export class BkdRadixSelector {
public:
    explicit BkdRadixSelector(HeapPointWriter &writer) : writer_(&writer) {}

    /// Partitions `[from, to)` at `partition_point` using dimension `dim`
    /// (a byte offset inside the packed point), skipping the first
    /// `dim_common_prefix` bytes of that dimension.
    ///
    /// Returns the full `bytes_per_dim` value at the partition point.
    ///
    /// The returned value is the *smallest* value that separates the two sides:
    /// the skipped prefix bytes and everything past the byte the descent stopped
    /// on are zero, so the value does not depend on the arbitrary low bytes of
    /// whichever point happens to sit at the boundary.  The packed index relies
    /// on that being a stable anchor.
    [[nodiscard]] Result<std::vector<uint8_t>> select(size_t from, size_t to, size_t partition_point, size_t dim,
                                                      size_t dim_common_prefix = 0);

    /// Sorts `[from, to)` by `dim`, skipping `dim_common_prefix` bytes.
    [[nodiscard]] VoidResult sort(size_t from, size_t to, size_t dim, size_t dim_common_prefix = 0);

private:
    /// Permutes `[from, to)` so that equal bytes at `byte_index` are adjacent.
    ///
    /// Returns the 257 absolute bucket boundaries: bucket `b` is
    /// `[boundaries[b], boundaries[b + 1])`.
    [[nodiscard]] std::array<size_t, 257> radix_partition(size_t from, size_t to, size_t byte_index);

    /// In-place MSD radix sort of `[start, end)` starting at `byte`.
    void radix_sort(size_t start, size_t end, size_t byte);

    void swap_points(size_t i, size_t j) { writer_->swap(i, j); }
    /// Value at `index`, keeping the bytes the descent actually used
    /// (`partitioned_through` is the first byte it did not partition).
    [[nodiscard]] std::vector<uint8_t> value_at(size_t index, size_t partitioned_through);

    HeapPointWriter *writer_;
    size_t dim_{0};
    size_t prefix_{0};
    size_t compare_length_{0};
};

} // namespace lucene
