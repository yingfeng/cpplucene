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

module lucene_core:long_heap.impl;

import :long_heap;

import :types;
import std;

import :error;
import :array_util;
import :debug;

namespace lucene {

Result<LongHeap> LongHeap::create(size_t max_size) {
    if (max_size == 0 || max_size >= ArrayUtil::MAX_ARRAY_LENGTH) {
        return err(LuceneError::illegal_argument(
                std::format("max_size must be > 0 and < {}; got: {}", ArrayUtil::MAX_ARRAY_LENGTH, max_size)));
    }
    // +1 because index 0 is unused.
    std::vector<int64_t> heap(max_size + 1, 0);
    return LongHeap(max_size, std::move(heap));
}

Result<int64_t> LongHeap::push(int64_t element) {
    ++size_;
    if (size_ == heap_.size()) {
        const size_t new_capacity = (size_ * 3 + 1) / 2; // div_ceil(1.5 * size)
        debug_check(new_capacity <= ArrayUtil::MAX_ARRAY_LENGTH, "heap growth overflow");
        auto grown = ArrayUtil::grow_with(heap_, new_capacity, [] { return 0; });
        if (!grown) {
            return std::unexpected(std::move(grown.error()));
        }
    }
    heap_[size_] = element;
    up_heap(size_);
    return heap_[1];
}

Result<bool> LongHeap::insert_with_overflow(int64_t value) {
    if (size_ >= max_size_) {
        if (value < heap_[1]) {
            return false;
        }
        update_top(value);
        return true;
    }
    auto pushed = push(value);
    if (!pushed) {
        return std::unexpected(std::move(pushed.error()));
    }
    return true;
}

Result<int64_t> LongHeap::pop() {
    if (size_ == 0) {
        return err(LuceneError::illegal_state("The heap is empty"));
    }
    const int64_t result = heap_[1];
    heap_[1] = heap_[size_];
    --size_;
    down_heap(1);
    return result;
}

int64_t LongHeap::update_top(int64_t value) noexcept {
    if (size_ == 0) {
        return value;
    }
    heap_[1] = value;
    down_heap(1);
    return heap_[1];
}

VoidResult LongHeap::push_all(const LongHeap &other) {
    for (size_t i = 1; i <= other.size_; ++i) {
        auto pushed = push(other.heap_[i]);
        if (!pushed) {
            return std::unexpected(std::move(pushed.error()));
        }
    }
    return {};
}

void LongHeap::up_heap(size_t i) {
    const int64_t value = heap_[i]; // save the bottom value
    while (i > 1) {
        const size_t parent_index = i >> 1;
        if (value >= heap_[parent_index]) {
            break;
        }
        heap_[i] = heap_[parent_index]; // shift parent down
        i = parent_index;
    }
    heap_[i] = value;
}

void LongHeap::down_heap(size_t i) noexcept {
    const int64_t value = heap_[i];
    size_t j = i << 1;
    size_t k = j + 1;

    if (k <= size_ && heap_[k] < heap_[j]) {
        j = k;
    }

    while (j <= size_ && heap_[j] < value) {
        heap_[i] = heap_[j];
        i = j;
        j = i << 1;
        k = j + 1;
        if (k <= size_ && heap_[k] < heap_[j]) {
            j = k;
        }
    }

    heap_[i] = value;
}

} // namespace lucene
