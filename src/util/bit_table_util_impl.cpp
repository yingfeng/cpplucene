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

module lucene_core:bit_table_util.impl;

import :bit_table_util;
import :types;
import :error;
import :debug;
import :fst_bytes_reader;

import std;

namespace lucene {

namespace {

/// Reads `num_bytes` (1 to 8) bytes little endian into a `uint64_t`.
Result<uint64_t> read_le(FstBytesReader &reader, int32_t num_bytes) {
    debug_check(num_bytes >= 0 && num_bytes <= 8, "at most 8 bytes can be read");
    uint64_t value = 0;
    for (int32_t i = 0; i < num_bytes; ++i) {
        auto byte = reader.read_byte();
        if (!byte.has_value()) {
            return err(byte.error());
        }
        value |= static_cast<uint64_t>(byte.value()) << (8 * i);
    }
    return value;
}

} // namespace

Result<bool> BitTableUtil::is_bit_set(int32_t bit_index, FstBytesReader &reader) {
    debug_check(bit_index >= 0, "bitIndex must be non-negative");
    if (auto result = reader.skip_bytes(bit_index >> 3); !result.has_value()) {
        return err(result.error());
    }
    auto byte = reader.read_byte();
    if (!byte.has_value()) {
        return err(byte.error());
    }
    const uint8_t mask = static_cast<uint8_t>(1u << (bit_index & 7));
    return (byte.value() & mask) != 0;
}

Result<int32_t> BitTableUtil::count_bits(int32_t bit_table_bytes, FstBytesReader &reader) {
    debug_check(bit_table_bytes >= 0, "bitTableBytes must be non-negative");
    int32_t count = 0;
    const int32_t num_long_blocks = bit_table_bytes >> 3;
    for (int32_t block = 0; block < num_long_blocks; ++block) {
        auto value = read_le(reader, 8);
        if (!value.has_value()) {
            return err(value.error());
        }
        count += std::popcount(value.value());
    }
    const int32_t remaining = bit_table_bytes & 7;
    if (remaining != 0) {
        auto value = read_le(reader, remaining);
        if (!value.has_value()) {
            return err(value.error());
        }
        count += std::popcount(value.value());
    }
    return count;
}

Result<int32_t> BitTableUtil::count_bits_upto(int32_t bit_index, FstBytesReader &reader) {
    debug_check(bit_index >= 0, "bitIndex must be non-negative");
    int32_t count = 0;
    const int32_t num_long_blocks = bit_index >> 6;
    for (int32_t block = 0; block < num_long_blocks; ++block) {
        auto value = read_le(reader, 8);
        if (!value.has_value()) {
            return err(value.error());
        }
        count += std::popcount(value.value());
    }
    const int32_t remaining_bits = bit_index & 63;
    if (remaining_bits != 0) {
        auto value = read_le(reader, 8);
        if (!value.has_value()) {
            return err(value.error());
        }
        const uint64_t mask = (uint64_t{1} << remaining_bits) - 1;
        count += std::popcount(value.value() & mask);
    }
    return count;
}

Result<int32_t> BitTableUtil::next_bit_set(int32_t bit_index, int32_t num_bytes, FstBytesReader &reader) {
    debug_check(bit_index >= 0, "bitIndex must be non-negative");
    const int32_t first_index = bit_index >> 3;
    int32_t index = first_index;
    if (auto result = reader.skip_bytes(index); !result.has_value()) {
        return err(result.error());
    }
    while (index < num_bytes) {
        auto byte = reader.read_byte();
        if (!byte.has_value()) {
            return err(byte.error());
        }
        uint8_t value = byte.value();
        if (index == first_index) {
            // Bits below `bit_index` inside the first byte do not count.
            value &= static_cast<uint8_t>(0xFFu << (bit_index & 7));
        }
        if (value != 0) {
            return (index << 3) + std::countr_zero(static_cast<uint32_t>(value));
        }
        ++index;
    }
    return -1;
}

Result<int32_t> BitTableUtil::previous_bit_set(int32_t bit_index, FstBytesReader &reader) {
    debug_check(bit_index >= 0, "bitIndex must be non-negative");
    int32_t byte_index = bit_index >> 3;
    if (auto result = reader.skip_bytes(byte_index); !result.has_value()) {
        return err(result.error());
    }
    const uint8_t mask = static_cast<uint8_t>((1u << (bit_index & 7)) - 1u);
    auto byte = reader.read_byte();
    if (!byte.has_value()) {
        return err(byte.error());
    }
    uint8_t value = static_cast<uint8_t>(byte.value() & mask);
    while (value == 0) {
        if (byte_index == 0) {
            return -1;
        }
        --byte_index;
        // The reader supports negative skips: step back over the byte just read.
        if (auto result = reader.skip_bytes(-2); !result.has_value()) {
            return err(result.error());
        }
        auto previous = reader.read_byte();
        if (!previous.has_value()) {
            return err(previous.error());
        }
        value = previous.value();
    }
    // Index of the highest set bit inside `value`, plus the byte offset.
    return (31 - static_cast<int32_t>(std::countl_zero(static_cast<uint32_t>(value)))) + (byte_index << 3);
}

} // namespace lucene
