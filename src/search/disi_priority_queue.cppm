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

export module lucene_core:disi_priority_queue;

import :types;
import std;

import :disi_wrapper;
import :error;
import :scorer;

namespace lucene {

/// A priority queue of [`DisiWrapper`]s that orders by the current document.
///
/// Port of `DisiPriorityQueue` (`core/search/disi_priority_queue.rs`).
///
/// The queue only stores the indices of the wrappers, so the wrappers stay where
/// the caller put them; every operation therefore takes the wrapper array.
export template <class S> class DisiPriorityQueue {
public:
    /// A queue for at most `max_size` wrappers.
    explicit DisiPriorityQueue(size_t max_size) : heap_(max_size, 0) {}

    [[nodiscard]] static size_t left_node(size_t node) noexcept { return ((node + 1) << 1) - 1; }

    [[nodiscard]] static size_t right_node(size_t node) noexcept { return node + 1; }

    [[nodiscard]] static std::optional<size_t> parent_node(size_t node) noexcept {
        if (node == 0) {
            return std::nullopt;
        }
        return (node - 1) >> 1;
    }

    [[nodiscard]] size_t size() const noexcept { return size_; }

    /// The index of the least wrapper, or `std::nullopt` when the queue is empty.
    [[nodiscard]] std::optional<size_t> top() const {
        if (size_ == 0) {
            return std::nullopt;
        }
        return heap_[0];
    }

    /// The index of the second least wrapper, or `std::nullopt` when the queue
    /// holds fewer than two.
    [[nodiscard]] std::optional<size_t> top2(std::span<const DisiWrapper<S>> wrappers) const {
        if (size_ <= 1) {
            return std::nullopt;
        }
        if (size_ == 2) {
            return heap_[1];
        }
        const size_t left = heap_[1];
        const size_t right = heap_[2];
        return wrappers[left].doc() <= wrappers[right].doc() ? left : right;
    }

    /// Links together every wrapper that sits on the top document, through
    /// [`DisiWrapper::set_next`], and answers the head of that list.
    size_t top_list_root(std::vector<DisiWrapper<S>> &wrappers) {
        size_t list_index = heap_[0];
        wrappers[list_index].set_next(std::nullopt);

        if (size_ >= 3) {
            list_index = top_list(list_index, wrappers, size_, 1);
            list_index = top_list(list_index, wrappers, size_, 2);
        } else if (size_ == 2) {
            const size_t child = heap_[1];
            if (wrappers[child].doc() == wrappers[list_index].doc()) {
                list_index = prepend(child, list_index, wrappers);
            }
        }
        return list_index;
    }

    /// Adds the wrapper at `entry` and answers the index of the new top.
    size_t add(size_t entry, std::span<const DisiWrapper<S>> wrappers) {
        heap_[size_] = entry;
        up_heap(size_, wrappers);
        ++size_;
        return heap_[0];
    }

    /// Adds `entries` at once and re-heapifies in bulk.
    [[nodiscard]] VoidResult add_all(std::span<const size_t> entries, std::span<const DisiWrapper<S>> wrappers) {
        const size_t len = entries.size();
        if (len == 0) {
            return {};
        }
        if (size_ + len > heap_.size()) {
            return err(LuceneError::array_index_out_of_bounds("cannot add " + std::to_string(len) +
                                                              " elements to a queue with remaining capacity " +
                                                              std::to_string(heap_.size() - size_)));
        }
        for (size_t index = 0; index < len; ++index) {
            heap_[size_ + index] = entries[index];
        }
        size_ += len;

        // Heapify in bulk.
        const size_t first_leaf_index = size_ >> 1;
        for (size_t root_index = first_leaf_index; root_index > 0; --root_index) {
            size_t parent_index = root_index - 1;
            const size_t parent = heap_[parent_index];
            const int32_t parent_doc = wrappers[parent].doc();

            while (parent_index < first_leaf_index) {
                size_t child_index = left_node(parent_index);
                const size_t right_child_index = right_node(child_index);

                size_t child = heap_[child_index];
                if (right_child_index < size_ && wrappers[heap_[right_child_index]].doc() < wrappers[child].doc()) {
                    child = heap_[right_child_index];
                    child_index = right_child_index;
                }
                if (wrappers[child].doc() >= parent_doc) {
                    break;
                }
                heap_[parent_index] = child;
                parent_index = child_index;
            }
            heap_[parent_index] = parent;
        }
        return {};
    }

    /// Removes the least wrapper and answers its index.
    size_t pop(std::span<const DisiWrapper<S>> wrappers) {
        const auto result = top();
        if (!result.has_value()) {
            return 0;
        }
        --size_;
        heap_[0] = heap_[size_];
        heap_[size_] = 0;
        down_heap(size_, wrappers);
        return result.value();
    }

    /// Re-heapifies after the top wrapper moved forward, and answers the new top.
    size_t update_top(std::span<const DisiWrapper<S>> wrappers) {
        down_heap(size_, wrappers);
        return heap_[0];
    }

    /// Replaces the top wrapper with `top_replacement` and re-heapifies.
    size_t update_top_with(size_t top_replacement, std::span<const DisiWrapper<S>> wrappers) {
        heap_[0] = top_replacement;
        return update_top(wrappers);
    }

    /// The indices of the wrappers in the queue, in heap order.
    [[nodiscard]] std::span<const size_t> entries() const noexcept {
        return std::span<const size_t>(heap_.data(), size_);
    }

    /// Empties the queue.
    void clear() {
        std::ranges::fill(heap_, 0);
        size_ = 0;
    }

private:
    size_t prepend(size_t w1_index, size_t w2_index, std::vector<DisiWrapper<S>> &wrappers) {
        wrappers[w1_index].set_next(w2_index);
        return w1_index;
    }

    size_t top_list(size_t list, std::vector<DisiWrapper<S>> &wrappers, size_t size, size_t i) {
        const size_t w_index = heap_[i];
        if (wrappers[w_index].doc() != wrappers[list].doc()) {
            return list;
        }
        list = prepend(w_index, list, wrappers);

        const size_t left = left_node(i);
        const size_t right = left + 1;
        if (right < size) {
            list = top_list(list, wrappers, size, left);
            list = top_list(list, wrappers, size, right);
        } else if (left < size) {
            const size_t left_index = heap_[left];
            if (wrappers[left_index].doc() == wrappers[list].doc()) {
                list = prepend(left_index, list, wrappers);
            }
        }
        return list;
    }

    void up_heap(size_t i, std::span<const DisiWrapper<S>> wrappers) {
        const size_t node_index = heap_[i];
        const int32_t node_doc = wrappers[node_index].doc();
        for (;;) {
            const auto parent = parent_node(i);
            if (!parent.has_value() || node_doc >= wrappers[heap_[parent.value()]].doc()) {
                break;
            }
            heap_[i] = heap_[parent.value()];
            i = parent.value();
        }
        heap_[i] = node_index;
    }

    void down_heap(size_t size, std::span<const DisiWrapper<S>> wrappers) {
        if (size == 0) {
            return;
        }
        size_t i = 0;
        const size_t node = heap_[0];
        size_t j = left_node(i);
        if (j >= size) {
            return;
        }
        size_t k = right_node(j);
        if (k < size && wrappers[heap_[k]].doc() < wrappers[heap_[j]].doc()) {
            j = k;
        }
        if (wrappers[heap_[j]].doc() < wrappers[node].doc()) {
            for (;;) {
                heap_[i] = heap_[j];
                i = j;
                j = left_node(i);
                k = right_node(j);
                if (k < size && wrappers[heap_[k]].doc() < wrappers[heap_[j]].doc()) {
                    j = k;
                }
                if (j >= size || wrappers[heap_[j]].doc() >= wrappers[node].doc()) {
                    break;
                }
            }
            heap_[i] = node;
        }
    }

    size_t size_{0};
    std::vector<size_t> heap_;
};

} // namespace lucene
