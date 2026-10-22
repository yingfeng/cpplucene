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

module lucene_core:bytes_ref_block_pool.impl;

import :bytes_ref_block_pool;

import :bit_util;
import :byte_block_pool;
import :bytes_ref;
import :error;
import :string_helper;
import :types;

import std;

namespace lucene {

namespace {

/// `BytesRefHash::do_hash`: one-byte terms use a lookup table, longer terms
/// use MurmurHash3 (so that the hash of a term equals `BytesRef::hash_code`).
int32_t do_hash(std::span<const uint8_t> bytes, size_t offset, size_t length) {
    static const std::array<int32_t, 256> single_byte_hashes = [] {
        std::array<int32_t, 256> table{};
        for (size_t i = 0; i < table.size(); ++i) {
            const uint8_t single[1] = {static_cast<uint8_t>(i)};
            table[i] = StringHelper::murmurhash3_x86_32(std::span<const uint8_t>(single, 1), 0, 1,
                                                        StringHelper::good_fast_hash_seed());
        }
        return table;
    }();

    if (length == 1) {
        return single_byte_hashes[bytes[offset]];
    }
    return StringHelper::murmurhash3_x86_32(bytes, offset, length, StringHelper::good_fast_hash_seed());
}

/// Reads the `(length, offset)` prefix of the term stored at `pos`.
std::pair<size_t, size_t> read_prefix(std::span<const uint8_t> block, size_t pos) {
    if ((block[pos] & 0x80) == 0) {
        // Length fits in one byte.
        return {static_cast<size_t>(block[pos]), pos + 1};
    }
    // Two-byte big-endian length, high bit used as the marker.
    return {static_cast<size_t>(BitUtil::get_i16_be(block, pos) & 0x7FFF), pos + 2};
}

} // namespace

void BytesRefBlockPool::reset(ByteBlockPool &byte_block_pool) { byte_block_pool.reset(false, false); }

BytesRefBlockPoolPosition BytesRefBlockPool::fill_bytes_ref(int32_t start, const ByteBlockPool &byte_block_pool) const {
    const auto block_index = static_cast<size_t>(start >> BYTE_BLOCK_SHIFT);
    const auto pos = static_cast<size_t>(start & BYTE_BLOCK_MASK);
    const auto [length, offset] = read_prefix(byte_block_pool.buffer(block_index), pos);
    return BytesRefBlockPoolPosition{block_index, offset, length};
}

Result<BytesRef> BytesRefBlockPool::bytes_ref(const BytesRefBlockPoolPosition &position,
                                              const ByteBlockPool &byte_block_pool) const {
    const auto &block = byte_block_pool.buffer(position.block_index);
    if (position.offset + position.length > block.size()) {
        return err(LuceneError::corrupt_index("term crosses the end of its block"));
    }
    return BytesRef(std::vector<uint8_t>(block.begin() + static_cast<std::ptrdiff_t>(position.offset),
                                         block.begin() +
                                                 static_cast<std::ptrdiff_t>(position.offset + position.length)));
}

Result<int32_t> BytesRefBlockPool::add_bytes_ref(const BytesRef &bytes, ByteBlockPool &pool) {
    const auto slice = bytes.as_slice();
    const auto length = static_cast<int32_t>(slice.size());
    const int32_t len2 = 2 + length;
    if (len2 + pool.byte_upto() > BYTE_BLOCK_SIZE) {
        if (len2 > BYTE_BLOCK_SIZE) {
            return err(LuceneError::max_bytes_length_exceeded(
                    "bytes can be at most " + std::to_string(BYTE_BLOCK_SIZE - 2) + " in length; got " +
                    std::to_string(length)));
        }
        if (auto next = pool.next_buffer(); !next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
    }

    const int32_t text_start = pool.byte_upto() + pool.byte_offset();
    const auto buffer_upto = static_cast<size_t>(pool.byte_upto());
    const auto buffer_index = pool.current_buffer();
    if (!buffer_index.has_value()) {
        return err(LuceneError::illegal_state("byte block pool has no head buffer"));
    }
    auto &block = pool.mutable_buffer(buffer_index.value());

    // The length is encoded as a 1 or 2 byte prefix (longer terms are rejected
    // above), followed by the bytes themselves.
    int32_t new_length = 0;
    if (length < 128) {
        block[buffer_upto] = static_cast<uint8_t>(length);
        std::copy(slice.begin(), slice.end(), block.begin() + static_cast<std::ptrdiff_t>(buffer_upto + 1));
        new_length = length + 1;
    } else {
        BitUtil::set_i16_be(block, buffer_upto, static_cast<int16_t>(length | 0x8000));
        std::copy(slice.begin(), slice.end(), block.begin() + static_cast<std::ptrdiff_t>(buffer_upto + 2));
        new_length = length + 2;
    }
    pool.set_byte_upto(pool.byte_upto() + new_length);
    return text_start;
}

int32_t BytesRefBlockPool::hash(int32_t start, const ByteBlockPool &pool) const {
    const auto block_index = static_cast<size_t>(start >> BYTE_BLOCK_SHIFT);
    const auto pos = static_cast<size_t>(start & BYTE_BLOCK_MASK);
    const auto &block = pool.buffer(block_index);
    const auto [length, offset] = read_prefix(block, pos);
    return do_hash(block, offset, length);
}

bool BytesRefBlockPool::equals(int32_t start, const BytesRef &b, const ByteBlockPool &pool) const {
    const auto block_index = static_cast<size_t>(start >> BYTE_BLOCK_SHIFT);
    const auto pos = static_cast<size_t>(start & BYTE_BLOCK_MASK);
    const auto &block = pool.buffer(block_index);
    const auto [length, offset] = read_prefix(block, pos);
    const auto expected = b.as_slice();
    if (expected.size() != length) {
        return false;
    }
    return std::equal(expected.begin(), expected.end(), block.begin() + static_cast<std::ptrdiff_t>(offset));
}

} // namespace lucene
