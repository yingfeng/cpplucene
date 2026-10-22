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

module lucene_core:byte_slice_reader.impl;

import :byte_slice_reader;

import :bit_util;
import :byte_block_pool;
import :byte_slice_pool;
import :debug;
import :error;
import :types;

import std;

namespace lucene {

void ByteSliceReader::init(size_t start_index, size_t end_index) {
    debug_check(end_index >= start_index, "the end of a slice must not precede its start");

    end_index_ = end_index;
    level_ = 0;

    buffer_upto_ = start_index / static_cast<size_t>(BYTE_BLOCK_SIZE);
    buffer_offset_ = buffer_upto_ * static_cast<size_t>(BYTE_BLOCK_SIZE);
    upto_ = start_index & static_cast<size_t>(BYTE_BLOCK_MASK);

    const auto first_size = static_cast<size_t>(ByteSlicePool::LEVEL_SIZE_ARRAY[0]);

    if (start_index + first_size >= end_index) {
        // A single slice: read up to the end of the data.
        limit_ = end_index & static_cast<size_t>(BYTE_BLOCK_MASK);
    } else {
        limit_ = upto_ + first_size - 4;
    }
}

bool ByteSliceReader::eof() const {
    debug_check(upto_ + buffer_offset_ <= end_index_, "read past the end of the slices");
    return upto_ + buffer_offset_ == end_index_;
}

VoidResult ByteSliceReader::next_slice() {
    const auto &buffer = pool_.buffer(buffer_upto_);
    const int64_t next_index = BitUtil::get_i32_le(buffer, limit_);

    level_ = ByteSlicePool::NEXT_LEVEL_ARRAY[level_];
    const auto new_size = static_cast<size_t>(ByteSlicePool::LEVEL_SIZE_ARRAY[level_]);

    const auto next = static_cast<size_t>(next_index);
    buffer_upto_ = next / static_cast<size_t>(BYTE_BLOCK_SIZE);
    buffer_offset_ = buffer_upto_ * static_cast<size_t>(BYTE_BLOCK_SIZE);
    upto_ = next & static_cast<size_t>(BYTE_BLOCK_MASK);

    if (next + new_size >= end_index_) {
        // Final slice.
        debug_check(end_index_ - next > 0, "the final slice must not be empty");
        limit_ = end_index_ - buffer_offset_;
    } else {
        // Intermediate slice: reserve 4 bytes for the forwarding address.
        limit_ = upto_ + new_size - 4;
    }
    return {};
}

size_t ByteSliceReader::write(DataOutput &out) {
    size_t size = 0;
    while (true) {
        const auto &buffer = pool_.buffer(buffer_upto_);
        const size_t len = limit_ - upto_;
        const std::span<const uint8_t> chunk(std::span<const uint8_t>(buffer).subspan(upto_, len));
        if (auto written = out.write_bytes(chunk, 0, len); !written.has_value()) {
            return size;
        }
        size += len;
        if (limit_ + buffer_offset_ == end_index_) {
            break;
        }
        if (auto next = next_slice(); !next.has_value()) {
            return size;
        }
    }
    return size;
}

Result<uint8_t> ByteSliceReader::read_byte() {
    debug_check(!eof(), "read past the end of the slices");
    debug_check(upto_ <= limit_, "read past the end of a slice");

    if (upto_ == limit_) {
        if (auto next = next_slice(); !next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
    }

    const uint8_t byte = pool_.buffer(buffer_upto_)[upto_];
    ++upto_;
    return byte;
}

VoidResult ByteSliceReader::read_bytes(std::span<uint8_t> b) {
    size_t offset = 0;
    size_t len = b.size();
    while (len > 0) {
        const size_t num_left = limit_ - upto_;
        if (num_left < len) {
            const auto &buffer = pool_.buffer(buffer_upto_);
            std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(upto_), num_left,
                        b.begin() + static_cast<std::ptrdiff_t>(offset));
            offset += num_left;
            len -= num_left;
            if (auto next = next_slice(); !next.has_value()) {
                return next;
            }
        } else {
            const auto &buffer = pool_.buffer(buffer_upto_);
            std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(upto_), len,
                        b.begin() + static_cast<std::ptrdiff_t>(offset));
            upto_ += len;
            break;
        }
    }
    return {};
}

VoidResult ByteSliceReader::skip_bytes(int64_t num_bytes) {
    if (num_bytes < 0) {
        return err(LuceneError::illegal_argument("num_bytes must be >= 0, got " + std::to_string(num_bytes)));
    }
    auto remaining = static_cast<size_t>(num_bytes);
    while (remaining > 0) {
        const size_t num_left = limit_ - upto_;
        if (num_left < remaining) {
            remaining -= num_left;
            if (auto next = next_slice(); !next.has_value()) {
                return next;
            }
        } else {
            upto_ += remaining;
            break;
        }
    }
    return {};
}

} // namespace lucene
