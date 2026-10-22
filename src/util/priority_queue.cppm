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

export module lucene_core:priority_queue;

import :types;

import std;

import :error;

namespace lucene {

/// A priority queue that keeps its least element reachable in constant time.
///
/// Port of `PriorityQueue` (`core/util/priority_queue.rs`).
///
/// The heap is 1-based exactly like rlucene's (`heap[0]` is unused), and it can
/// be pre-filled with sentinel elements so that users such as the hit queue can
/// assume it is always full and only ever replace the top.
///
/// **Deviations** (C++ mechanics only):
///  * the `Compare<T>` trait becomes a template parameter: `C` must provide
///    `Result<bool> less_than(const T &a, const T &b) const`, which is where
///    rlucene's `Compare::less_than` lands;
///  * `top` / `top_mut` return pointers instead of `Option<&T>`/`Option<&mut T>`
///    (a null pointer means the queue is empty);
///  * `iter_ref` / `iter` become [`elements`] and [`take_all`], which is all the
///    ported callers need;
///  * `CompareEnum2` is not ported; it only appears in the generic collector
///    machinery, which the port collapses into concrete types.
export template <class T, class C>
class PriorityQueue {
public:
    /// Creates a queue pre-filled with sentinel elements.
    ///
    /// `supplier` is called once per slot and must return a value every time
    /// after it returned one once; the sentinels must compare worse than any
    /// real element, i.e. `less_than(sentinel, real)` must be `true`.
    template <class F>
    static Result<PriorityQueue> with_sentinel_object(size_t max_size, F &&supplier, C compare) {
        size_t heap_size;
        if (max_size == 0) {
            // Allocate one extra slot so `top()` never needs a bounds check.
            heap_size = 2;
        } else {
            if (max_size >= static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
                return err(LuceneError::illegal_argument("maxSize must be >= 0 and < " +
                                                         std::to_string(std::numeric_limits<int32_t>::max()) +
                                                         "; got: " + std::to_string(max_size)));
            }
            // Index 0 is unused: all access is 1-based.
            heap_size = max_size + 1;
        }

        std::vector<std::optional<T>> heap(heap_size);
        auto first = supplier();
        if (first.has_value()) {
            heap[1] = std::move(first);
            for (size_t i = 2; i < heap_size; ++i) {
                auto next = supplier();
                if (!next.has_value()) {
                    return err(LuceneError::illegal_state("sentinel_object_supplier must not return None"));
                }
                heap[i] = std::move(next.value());
            }
            return PriorityQueue(max_size, std::move(heap), max_size, std::move(compare));
        }
        return PriorityQueue(max_size, std::move(heap), 0, std::move(compare));
    }

    /// Creates an empty queue of at most `max_size` elements.
    static Result<PriorityQueue> create(size_t max_size, C compare) {
        return with_sentinel_object(max_size, [] -> std::optional<T> { return std::nullopt; }, std::move(compare));
    }

    /// The least element, or null when the queue is empty.
    [[nodiscard]] const T *top() const noexcept {
        return heap_[1].has_value() ? &heap_[1].value() : nullptr;
    }

    /// The least element, writable; null when the queue is empty.
    [[nodiscard]] T *top_mut() noexcept { return heap_[1].has_value() ? &heap_[1].value() : nullptr; }

    /// Number of elements currently stored.
    [[nodiscard]] size_t size() const noexcept { return size_; }

    /// The maximum number of elements.
    [[nodiscard]] size_t max_size() const noexcept { return max_size_; }

    /// Removes every element.
    void clear() noexcept {
        for (size_t i = 1; i <= size_ && i < heap_.size(); ++i) {
            heap_[i].reset();
        }
        size_ = 0;
    }

    /// The stored elements, in unspecified order.
    [[nodiscard]] std::vector<const T *> elements() const {
        std::vector<const T *> result;
        result.reserve(size_);
        for (size_t i = 1; i <= size_; ++i) {
            if (heap_[i].has_value()) {
                result.push_back(&heap_[i].value());
            }
        }
        return result;
    }

    /// Moves the stored elements out of the queue, leaving it empty.
    [[nodiscard]] std::vector<T> take_all() {
        std::vector<T> result;
        result.reserve(size_);
        for (size_t i = 1; i <= size_; ++i) {
            if (heap_[i].has_value()) {
                result.push_back(std::move(heap_[i].value()));
                heap_[i].reset();
            }
        }
        size_ = 0;
        return result;
    }

    /// Adds all elements, building the heap in linear time.
    [[nodiscard]] VoidResult add_all(std::vector<T> elements) {
        if (size_ + elements.size() > max_size_) {
            return err(LuceneError::array_index_out_of_bounds(
                    "Cannot add " + std::to_string(elements.size()) + " elements to a queue with remaining capacity: " +
                    std::to_string(max_size_ - size_)));
        }
        // The heap is a prefix of the array, so filling further is safe.
        for (auto &element : elements) {
            heap_[size_ + 1] = std::move(element);
            ++size_;
        }
        for (size_t i = size_ >> 1; i >= 1; --i) {
            if (auto down = down_heap(i); !down.has_value()) {
                return down;
            }
        }
        return {};
    }

    /// Adds one element and returns the new top, or fails when the queue is
    /// full.
    [[nodiscard]] Result<const T *> add(T element) {
        const size_t index = size_ + 1;
        // `heap_` has one extra slot when `max_size_` is 0, so the capacity has
        // to be checked against `max_size_` rather than against the array bound.
        if (max_size_ == 0 || index >= heap_.size()) {
            return err(LuceneError::array_index_out_of_bounds(
                    "Cannot add an element to a queue with remaining capacity: " +
                    std::to_string(max_size_ > size_ ? max_size_ - size_ : 0)));
        }
        heap_[index] = std::move(element);
        size_ = index;
        if (auto up = up_heap(index); !up.has_value()) {
            return std::unexpected(std::move(up.error()));
        }
        return top();
    }

    /// Adds one element, returning the element that was dropped because the
    /// queue was full (which may be `element` itself).
    [[nodiscard]] Result<std::optional<T>> insert_with_overflow(T element) {
        if (size_ < max_size_) {
            if (auto added = add(std::move(element)); !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
            return std::optional<T>{};
        }
        if (size_ > 0) {
            const T *current_top = top();
            auto less = compare_.less_than(*current_top, element);
            if (!less.has_value()) {
                return std::unexpected(std::move(less.error()));
            }
            if (less.value()) {
                T dropped = std::move(heap_[1].value());
                heap_[1] = std::move(element);
                if (auto updated = update_top(); !updated.has_value()) {
                    return std::unexpected(std::move(updated.error()));
                }
                return std::optional<T>(std::move(dropped));
            }
            return std::optional<T>(std::move(element));
        }
        return std::optional<T>(std::move(element));
    }

    /// Removes and returns the least element.
    [[nodiscard]] Result<std::optional<T>> pop() {
        if (size_ == 0) {
            return std::optional<T>{};
        }
        auto popped = pop_unchecked();
        if (!popped.has_value()) {
            return std::unexpected(std::move(popped.error()));
        }
        return std::optional<T>(std::move(popped.value()));
    }

    /// Removes and returns the least element; the caller guarantees the queue
    /// is not empty.
    [[nodiscard]] Result<T> pop_unchecked() {
        if (size_ == 0) {
            return err(LuceneError::illegal_state("pop_unchecked called on an empty queue"));
        }
        std::swap(heap_[1], heap_[size_]);
        if (!heap_[size_].has_value()) {
            return err(LuceneError::illegal_state("priority queue element should exist"));
        }
        T result = std::move(heap_[size_].value());
        heap_[size_].reset();
        --size_;
        if (auto down = down_heap(1); !down.has_value()) {
            return std::unexpected(std::move(down.error()));
        }
        return result;
    }

    /// Restores the heap after the top element changed; returns the new top.
    [[nodiscard]] Result<T *> update_top() {
        if (auto down = down_heap(1); !down.has_value()) {
            return std::unexpected(std::move(down.error()));
        }
        return top_mut();
    }

    /// Replaces the top element and restores the heap.
    [[nodiscard]] Result<T *> update_top_with_new_top(T new_top) {
        heap_[1] = std::move(new_top);
        return update_top();
    }

    /// Removes `element` in linear time; returns whether it was there.
    [[nodiscard]] Result<bool> remove(const T &element)
        requires std::equality_comparable<T>
    {
        for (size_t i = 1; i <= size_; ++i) {
            if (heap_[i].has_value() && heap_[i].value() == element) {
                std::swap(heap_[i], heap_[size_]);
                heap_[size_].reset();
                --size_;
                if (i <= size_) {
                    auto up = up_heap(i);
                    if (!up.has_value()) {
                        return std::unexpected(std::move(up.error()));
                    }
                    if (!up.value()) {
                        if (auto down = down_heap(i); !down.has_value()) {
                            return std::unexpected(std::move(down.error()));
                        }
                    }
                }
                return true;
            }
        }
        return false;
    }

    /// Moves `i` up until the heap property holds; returns whether it moved.
    [[nodiscard]] VoidResult up_heap(size_t orig_pos) {
        size_t i = orig_pos;
        size_t j = i >> 1;
        while (j > 0) {
            const T &value = heap_value(i);
            const T &parent = heap_value(j);
            auto less = compare_.less_than(value, parent);
            if (!less.has_value()) {
                return std::unexpected(std::move(less.error()));
            }
            if (!less.value()) {
                break;
            }
            std::swap(heap_[i], heap_[j]);
            i = j;
            j = i >> 1;
        }
        return {};
    }

    /// Moves `i` down until the heap property holds.
    [[nodiscard]] VoidResult down_heap(size_t i) {
        while (i * 2 <= size_) {
            size_t j = i * 2;
            const size_t k = j + 1;
            if (k <= size_) {
                auto right_less = compare_.less_than(heap_value(k), heap_value(j));
                if (!right_less.has_value()) {
                    return std::unexpected(std::move(right_less.error()));
                }
                if (right_less.value()) {
                    j = k;
                }
            }
            auto child_less = compare_.less_than(heap_value(j), heap_value(i));
            if (!child_less.has_value()) {
                return std::unexpected(std::move(child_less.error()));
            }
            if (!child_less.value()) {
                break;
            }
            std::swap(heap_[i], heap_[j]);
            i = j;
        }
        return {};
    }

private:
    PriorityQueue(size_t max_size, std::vector<std::optional<T>> heap, size_t size, C compare)
        : size_(size), max_size_(max_size), heap_(std::move(heap)), compare_(std::move(compare)) {}

    [[nodiscard]] const T &heap_value(size_t index) const { return heap_[index].value(); }

    size_t size_;
    size_t max_size_;
    std::vector<std::optional<T>> heap_;
    C compare_;
};

} // namespace lucene
