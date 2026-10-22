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

export module lucene_core:long_heap;

import :types;
import std;

import :error;

namespace lucene {

/// A min heap that stores `int64_t` values.  Port of `LongHeap`
/// (`core/util/long_heap.rs`).
///
/// This is a primitive priority queue that maintains a partial ordering of
/// elements such that the smallest element can always be found in constant
/// time.  `push()` and `pop()` require `O(log(n))`; the heap supports both
/// unbounded growth (via [`push`]) and bounded-size insertion (via
/// [`insert_with_overflow`]).
///
/// The heap is 1-based internally: index 0 is unused.
export class LongHeap {
public:
    /// Creates an empty priority queue of the configured initial size.
    ///
    /// Returns `illegal_argument` when `max_size` is 0 or not smaller than
    /// `ArrayUtil::MAX_ARRAY_LENGTH`, to prevent confusing out-of-memory
    /// errors.
    [[nodiscard]] static Result<LongHeap> create(size_t max_size);

    LongHeap(LongHeap &&) noexcept = default;
    LongHeap &operator=(LongHeap &&) noexcept = default;
    LongHeap(const LongHeap &) = delete;
    LongHeap &operator=(const LongHeap &) = delete;

    /// Adds a value in `O(log(n))`.  Grows unbounded as needed to accommodate
    /// new values.  Returns the new top element.
    [[nodiscard]] Result<int64_t> push(int64_t element);

    /// Adds a value in `O(log(n))`.  If the number of values would exceed
    /// `max_size`, the least value is discarded.  Returns whether the value
    /// was added.
    [[nodiscard]] Result<bool> insert_with_overflow(int64_t value);

    /// Returns the least element of the heap in constant time.  The caller
    /// must ensure the heap is not empty.
    [[nodiscard]] int64_t top() const noexcept { return heap_[1]; }

    /// Removes and returns the least element of the heap in `O(log(n))`.
    [[nodiscard]] Result<int64_t> pop();

    /// Replaces the top of the heap with `value`.  This is faster than `pop()`
    /// followed by `push()`.  Does nothing when the heap is empty.
    int64_t update_top(int64_t value) noexcept;

    /// Number of elements currently stored in the heap.
    [[nodiscard]] size_t size() const noexcept { return size_; }

    /// Removes all entries from the heap.
    void clear() noexcept { size_ = 0; }

    /// Pushes all elements of `other` into this heap.
    [[nodiscard]] VoidResult push_all(const LongHeap &other);

    /// Returns the element at the `i`-th location in the heap array.  Valid
    /// indices are in `[1, size]`.
    [[nodiscard]] int64_t get(size_t i) const { return heap_[i]; }

    /// The internal heap array (`heap[0]` is unused) — used by tests.
    [[nodiscard]] std::span<const int64_t> heap_array() const noexcept { return heap_; }

private:
    LongHeap(size_t max_size, std::vector<int64_t> heap) : max_size_(max_size), heap_(std::move(heap)) {}

    void up_heap(size_t i);
    void down_heap(size_t i) noexcept;

    size_t max_size_;
    std::vector<int64_t> heap_;
    size_t size_{0};
};

} // namespace lucene
