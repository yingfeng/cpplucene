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

module lucene_core:int_block_pool.impl;

import :int_block_pool;

import :debug;
import :error;
import :types;

import std;

namespace lucene {

std::vector<int32_t> DirectIntBlockAllocator::get_int_block() {
    return std::vector<int32_t>(static_cast<size_t>(INT_BLOCK_SIZE), 0);
}

IntBlockPool::IntBlockPool() : owned_allocator_(std::make_unique<DirectIntBlockAllocator>()) {
    allocator_ = owned_allocator_.get();
}

IntBlockPool::IntBlockPool(std::unique_ptr<IntBlockAllocator> allocator) : allocator_(allocator.get()) {
    if (allocator != nullptr) {
        owned_allocator_ = std::move(allocator);
    } else {
        owned_allocator_ = std::make_unique<DirectIntBlockAllocator>();
        allocator_ = owned_allocator_.get();
    }
}

void IntBlockPool::reset(bool zero_fill_buffers, bool reuse_first) {
    if (!buffer_upto_.has_value()) {
        return;
    }
    const size_t upto = buffer_upto_.value();
    if (zero_fill_buffers) {
        for (size_t i = 0; i < upto; ++i) {
            std::fill(buffers_[i].begin(), buffers_[i].end(), int32_t{0});
        }
        // Partial zero fill of the final block.
        std::fill(buffers_[upto].begin(), buffers_[upto].begin() + int_upto_, int32_t{0});
    }
    if (upto > 0 || !reuse_first) {
        const size_t offset = reuse_first ? 1 : 0;
        if (offset <= upto) {
            allocator_->recycle_int_blocks(buffers_, offset, upto + 1);
            for (size_t i = offset; i <= upto; ++i) {
                buffers_.pop_back();
            }
        }
    }

    if (reuse_first) {
        buffer_upto_ = 0;
        int_upto_ = 0;
        int_offset_ = 0;
    } else {
        buffer_upto_ = std::nullopt;
        int_upto_ = INT_BLOCK_SIZE;
        int_offset_ = -INT_BLOCK_SIZE;
    }
}

VoidResult IntBlockPool::next_buffer() {
    const size_t next_upto = buffer_upto_.has_value() ? buffer_upto_.value() + 1 : 0;
    if (next_upto == buffers_.size()) {
        buffers_.push_back(allocator_->get_int_block());
        debug_check(buffers_.back().size() == static_cast<size_t>(INT_BLOCK_SIZE),
                    "the allocator must return blocks of INT_BLOCK_SIZE slots");
    }
    buffer_upto_ = next_upto;
    int_upto_ = 0;
    const int64_t next_offset = static_cast<int64_t>(int_offset_) + INT_BLOCK_SIZE;
    if (next_offset > std::numeric_limits<int32_t>::max()) {
        return err(LuceneError::number_overflow("Overflow when calculating byte offset."));
    }
    int_offset_ = static_cast<int32_t>(next_offset);
    return {};
}

} // namespace lucene
