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

module lucene_core:byte_slice_pool.impl;

import :byte_slice_pool;

import :bit_util;
import :byte_block_pool;
import :error;
import :types;

import std;

namespace lucene {

Result<int32_t> ByteSlicePool::new_slice(int32_t size, ByteBlockPool &pool) {
    if (size > BYTE_BLOCK_SIZE) {
        return err(LuceneError::illegal_argument("Slice size " + std::to_string(size) +
                                                 " should be less than the block size " +
                                                 std::to_string(BYTE_BLOCK_SIZE)));
    }

    if (pool.byte_upto() > BYTE_BLOCK_SIZE - size) {
        if (auto next = pool.next_buffer(); !next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
    }
    const int32_t upto = pool.byte_upto();
    pool.set_byte_upto(upto + size);
    const auto buffer_upto = pool.current_buffer();
    if (!buffer_upto.has_value()) {
        return err(LuceneError::illegal_state("byte block pool has no head buffer"));
    }
    const auto byte_upto = static_cast<size_t>(pool.byte_upto());
    // The last byte of the slice codifies level 0.
    pool.mutable_buffer(buffer_upto.value())[byte_upto - 1] = 16;
    return upto;
}

Result<size_t> ByteSlicePool::alloc_slice(size_t slice_index, size_t upto, ByteBlockPool &pool) {
    auto allocated = alloc_known_size_slice(slice_index, upto, pool);
    if (!allocated.has_value()) {
        return std::unexpected(std::move(allocated.error()));
    }
    return static_cast<size_t>(static_cast<uint32_t>(allocated.value()) >> 8);
}

Result<int32_t> ByteSlicePool::alloc_known_size_slice(size_t slice_index, size_t upto, ByteBlockPool &pool) {
    // The last 4 bits of the slice's last byte codify its level.
    const uint8_t level_byte = pool.buffer(slice_index)[upto];
    const auto level = static_cast<size_t>(level_byte & 15);
    if (level >= NEXT_LEVEL_ARRAY.size()) {
        return err(LuceneError::corrupt_index("invalid slice level"));
    }
    const size_t new_level = NEXT_LEVEL_ARRAY[level];
    const int32_t new_size = LEVEL_SIZE_ARRAY[new_level];

    // Maybe allocate another block.
    if (pool.byte_upto() > BYTE_BLOCK_SIZE - new_size) {
        if (auto next = pool.next_buffer(); !next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
    }

    const int32_t new_upto = pool.byte_upto();
    const int64_t offset = static_cast<int64_t>(new_upto) + pool.byte_offset();
    if (offset > std::numeric_limits<int32_t>::max()) {
        return err(LuceneError::number_overflow("slice offset overflows an int"));
    }
    pool.set_byte_upto(new_upto + new_size);

    const auto buffer_upto = pool.current_buffer();
    if (!buffer_upto.has_value()) {
        return err(LuceneError::illegal_state("byte block pool has no head buffer"));
    }

    const int32_t past3 = BitUtil::get_i32_le(pool.buffer(slice_index), upto - 3) & 0xFFFFFF;
    {
        auto &current_buffer = pool.mutable_buffer(buffer_upto.value());
        debug_check(current_buffer[static_cast<size_t>(new_upto) + 3] == 0,
                    "the next slice must start with 4 zero bytes");
        BitUtil::set_i32_le(current_buffer, static_cast<size_t>(new_upto), past3);
    }

    // Write the forwarding address at the end of the previous slice.
    BitUtil::set_i32_le(pool.mutable_buffer(slice_index), upto - 3, static_cast<int32_t>(offset));

    // Write the new level marker.
    const auto byte_upto = static_cast<size_t>(pool.byte_upto());
    pool.mutable_buffer(buffer_upto.value())[byte_upto - 1] = static_cast<uint8_t>(16 | new_level);

    return static_cast<int32_t>(((static_cast<int64_t>(new_upto) + 3) << 8) | (new_size - 3));
}

} // namespace lucene
