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

export module lucene_core:sorter;

import :types;
import std;

import :error;

namespace lucene {

/// Below this size, a range is sorted by [`Sorter::binary_sort`].
export inline constexpr size_t BINARY_SORT_THRESHOLD = 20;
/// Below this size, a range is sorted by [`Sorter::insertion_sort`].
export inline constexpr size_t INSERTION_SORT_THRESHOLD = 16;

/// Checks that a `[from, to)` range is well formed.
export [[nodiscard]] VoidResult check_range(size_t from, size_t to);

/// Base class for the sorting algorithms of this port.
///
/// Port of `util/sorter.rs`.  A sorter never sees the values directly: it
/// compares and swaps *slots* of an array the caller owns, which is what lets
/// one algorithm sort terms, postings or documents depending on the subclass.
///
/// The default implementations of `compare`/`swap`/`set_pivot`/`compare_pivot`/
/// `sort` report `illegal_state`, exactly like the reference implementation: a subclass only has to
/// implement the operations its algorithm actually uses.
export class Sorter {
public:
    virtual ~Sorter() = default;

    /// Compares the values in slots `i` and `j`.
    virtual Result<int32_t> compare(size_t i, size_t j);

    /// Swaps the values in slots `i` and `j`.
    virtual VoidResult swap(size_t i, size_t j);

    /// Saves slot `i` as the pivot, for [`compare_pivot`].
    virtual VoidResult set_pivot(size_t i);

    /// Compares the saved pivot with slot `j`.
    virtual Result<int32_t> compare_pivot(size_t j);

    /// Sorts `[from, to)`.
    virtual VoidResult sort(size_t from, size_t to);

    // ---- algorithms shared by the subclasses ----

    /// Merges `[from, mid)` with `[mid, to)`, which must both be sorted.
    [[nodiscard]] VoidResult merge_in_place(size_t from, size_t mid, size_t to);

    /// First index in `[from, to)` whose value is not less than slot `val`.
    [[nodiscard]] Result<size_t> lower(size_t from, size_t to, size_t val);
    /// First index in `[from, to)` whose value is greater than slot `val`.
    [[nodiscard]] Result<size_t> upper(size_t from, size_t to, size_t val);
    /// [`lower`], galloping from the end (cheaper when `val` is near `to`).
    [[nodiscard]] Result<size_t> lower2(size_t from, size_t to, size_t val);
    /// [`upper`], galloping from the start (cheaper when `val` is near `from`).
    [[nodiscard]] Result<size_t> upper2(size_t from, size_t to, size_t val);

    [[nodiscard]] VoidResult reverse(size_t from, size_t to);
    /// Rotates `[lo, hi)` so that `[mid, hi)` comes first; requires
    /// `lo <= mid <= hi`.
    [[nodiscard]] VoidResult rotate(size_t lo, size_t mid, size_t hi);

    /// Stable binary insertion sort over `[from, to)`.
    [[nodiscard]] VoidResult binary_sort(size_t from, size_t to);
    [[nodiscard]] VoidResult binary_sort_with_start(size_t from, size_t to, size_t start);
    /// Stable insertion sort over `[from, to)`.
    [[nodiscard]] VoidResult insertion_sort(size_t from, size_t to);
    /// Heapsort over `[from, to)`; not stable.
    [[nodiscard]] VoidResult heap_sort(size_t from, size_t to);

protected:
    [[nodiscard]] VoidResult do_rotate(size_t lo, size_t mid, size_t hi);
    [[nodiscard]] VoidResult heapify(size_t from, size_t to);
    [[nodiscard]] VoidResult sift_down(size_t i, size_t from, size_t to);

    [[nodiscard]] static size_t heap_parent(size_t from, size_t i);
    [[nodiscard]] static size_t heap_child(size_t from, size_t i);
};

} // namespace lucene
