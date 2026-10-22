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

module lucene_core:byte_block_pool.impl;

import :byte_block_pool;
import :types;
import :error;
import :debug;
import :bytes_ref;

import std;

namespace lucene {

std::vector<uint8_t> DirectByteBlockAllocator::get_byte_block() {
    return std::vector<uint8_t>(static_cast<size_t>(BYTE_BLOCK_SIZE), 0);
}

ByteBlockPool::ByteBlockPool() : owned_allocator_(std::make_unique<DirectByteBlockAllocator>()) {
    allocator_ = owned_allocator_.get();
}

ByteBlockPool::ByteBlockPool(ByteBlockAllocator &allocator) : allocator_(&allocator) {}

void ByteBlockPool::reset(bool zero_fill_buffers, bool reuse_first) {
    if (!buffer_upto_.has_value()) {
        return;
    }
    const size_t upto = buffer_upto_.value();
    if (zero_fill_buffers) {
        for (size_t i = 0; i <= upto; ++i) {
            std::fill(buffers_[i].begin(), buffers_[i].end(), uint8_t{0});
        }
    }
    if (upto > 0 || !reuse_first) {
        const size_t offset = reuse_first ? 1 : 0;
        if (offset <= upto) {
            (*allocator_).recycle_byte_blocks(std::span<std::vector<uint8_t>>(buffers_.data() + offset, upto + 1 - offset));
            for (size_t i = offset; i <= upto; ++i) {
                buffers_.pop_back();
            }
        }
    }

    if (reuse_first) {
        buffer_upto_ = 0;
        byte_upto_ = 0;
        byte_offset_ = 0;
    } else {
        buffer_upto_ = std::nullopt;
        byte_upto_ = BYTE_BLOCK_SIZE;
        byte_offset_ = -BYTE_BLOCK_SIZE;
    }
}

Result<size_t> ByteBlockPool::next_buffer() {
    const size_t next_upto = buffer_upto_.has_value() ? buffer_upto_.value() + 1 : 0;
    if (next_upto == buffers_.size()) {
        buffers_.push_back((*allocator_).get_byte_block());
        debug_check(buffers_.back().size() == static_cast<size_t>(BYTE_BLOCK_SIZE),
                    "the allocator must return blocks of BYTE_BLOCK_SIZE bytes");
    }
    buffer_upto_ = next_upto;
    byte_upto_ = 0;
    const int64_t next_offset = static_cast<int64_t>(byte_offset_) + BYTE_BLOCK_SIZE;
    if (next_offset > std::numeric_limits<int32_t>::max()) {
        return err(LuceneError::number_overflow("Overflow when calculating byte offset."));
    }
    byte_offset_ = static_cast<int32_t>(next_offset);
    return next_upto;
}

VoidResult ByteBlockPool::append_range(std::span<const uint8_t> bytes, size_t offset, size_t length) {
    if (offset + length > bytes.size()) {
        return err(LuceneError::illegal_argument("append range is out of bounds"));
    }
    size_t written = 0;
    while (written < length) {
        if (!buffer_upto_.has_value() || byte_upto_ == BYTE_BLOCK_SIZE) {
            if (auto result = next_buffer(); !result.has_value()) {
                return err(result.error());
            }
        }
        const size_t buffer_left = static_cast<size_t>(BYTE_BLOCK_SIZE - byte_upto_);
        const size_t chunk = std::min(buffer_left, length - written);
        auto &block = buffers_[buffer_upto_.value()];
        std::copy_n(bytes.begin() + (offset + written), chunk, block.begin() + byte_upto_);
        byte_upto_ = static_cast<int32_t>(byte_upto_ + static_cast<int32_t>(chunk));
        written += chunk;
    }
    return {};
}

VoidResult ByteBlockPool::append(std::span<const uint8_t> bytes) {
    return append_range(bytes, 0, bytes.size());
}

VoidResult ByteBlockPool::append(const BytesRef &bytes) {
    const auto slice = bytes.as_slice();
    return append_range(slice, 0, slice.size());
}

VoidResult ByteBlockPool::append_from(const ByteBlockPool &source, int64_t src_offset, size_t length) {
    // Chunked copy so that no read spans more than one block.
    std::vector<uint8_t> chunk_bytes;
    size_t copied = 0;
    while (copied < length) {
        const int64_t offset = src_offset + static_cast<int64_t>(copied);
        const size_t pos = static_cast<size_t>(offset & BYTE_BLOCK_MASK);
        const size_t chunk = std::min(length - copied, static_cast<size_t>(BYTE_BLOCK_SIZE) - pos);
        chunk_bytes.resize(chunk);
        if (auto result = source.read_bytes(offset, chunk_bytes, 0, chunk); !result.has_value()) {
            return err(result.error());
        }
        if (auto result = append(chunk_bytes); !result.has_value()) {
            return result;
        }
        copied += chunk;
    }
    return {};
}

VoidResult ByteBlockPool::read_bytes(int64_t offset, std::span<uint8_t> dest, size_t dest_offset,
                                     size_t length) const {
    if (dest_offset + length > dest.size()) {
        return err(LuceneError::illegal_argument("read range is out of bounds"));
    }
    size_t read = 0;
    while (read < length) {
        const int64_t current = offset + static_cast<int64_t>(read);
        const size_t buffer_index = static_cast<size_t>(current >> BYTE_BLOCK_SHIFT);
        const size_t pos = static_cast<size_t>(current & BYTE_BLOCK_MASK);
        if (buffer_index >= buffers_.size()) {
            return err(LuceneError::illegal_argument("read past the end of the pool"));
        }
        const size_t chunk = std::min(length - read, static_cast<size_t>(BYTE_BLOCK_SIZE) - pos);
        std::copy_n(buffers_[buffer_index].begin() + pos, chunk, dest.begin() + dest_offset + read);
        read += chunk;
    }
    return {};
}

uint8_t ByteBlockPool::read_byte(size_t offset) const {
    const size_t buffer_index = offset >> BYTE_BLOCK_SHIFT;
    debug_check(buffer_index < buffers_.size(), "read past the end of the pool");
    return buffers_[buffer_index][offset & static_cast<size_t>(BYTE_BLOCK_MASK)];
}

Result<BytesRef> ByteBlockPool::bytes_ref(int64_t offset, size_t length) const {
    std::vector<uint8_t> bytes(length);
    if (auto result = read_bytes(offset, bytes, 0, length); !result.has_value()) {
        return err(result.error());
    }
    return BytesRef(std::move(bytes));
}

int64_t ByteBlockPool::get_bytes_used() const noexcept {
    return get_position();
}

} // namespace lucene
