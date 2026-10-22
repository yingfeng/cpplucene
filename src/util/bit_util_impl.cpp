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

module lucene_core:bit_util.impl;

import :bit_util;

import :types;
import :debug;
import std;

namespace lucene {

namespace {

template <class T>
[[nodiscard]] constexpr T bswap(T value) noexcept {
    if constexpr (sizeof(T) == 2) {
        return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(value)));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(value)));
    } else {
        return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(value)));
    }
}

} // namespace


namespace {

template <class T>
[[nodiscard]] T read_unaligned(std::span<const uint8_t> bytes, size_t pos) {
    debug_check(pos + sizeof(T) <= bytes.size());
    T value{};
    std::memcpy(&value, bytes.data() + pos, sizeof(T));
    return value;
}

template <class T>
void write_unaligned(std::span<uint8_t> bytes, size_t pos, T value, size_t len) {
    debug_check(pos + len <= bytes.size());
    debug_check(len <= sizeof(T));
    // Write `len` low-order bytes of the value in `T`'s memory order, which is
    // the byte-level copy semantics of the Rust implementation.
    std::memcpy(bytes.data() + pos, &value, len);
}

} // namespace

int32_t BitUtil::float_to_int_bits(float value) {
    if (std::isnan(value)) {
        return static_cast<int32_t>(FLOAT_NAN_BITS);
    }
    return std::bit_cast<int32_t>(value);
}

int64_t BitUtil::double_to_long_bits(double value) {
    if (std::isnan(value)) {
        return static_cast<int64_t>(DOUBLE_NAN_BITS);
    }
    return std::bit_cast<int64_t>(value);
}

// ---- big endian ----
int16_t BitUtil::get_i16_be(std::span<const uint8_t> bytes, size_t pos) {
    const uint16_t raw = static_cast<uint16_t>(read_unaligned<uint16_t>(bytes, pos));
    return static_cast<int16_t>(bswap(raw));
}

void BitUtil::set_i16_be(std::span<uint8_t> bytes, size_t pos, int16_t value) { set_i16_be(bytes, pos, value, SHORT_BYTES); }

void BitUtil::set_i16_be(std::span<uint8_t> bytes, size_t pos, int16_t value, size_t len) {
    write_unaligned(bytes, pos, bswap(value), len);
}

int32_t BitUtil::get_i32_be(std::span<const uint8_t> bytes, size_t pos) {
    const uint32_t raw = read_unaligned<uint32_t>(bytes, pos);
    return static_cast<int32_t>(bswap(raw));
}

void BitUtil::set_i32_be(std::span<uint8_t> bytes, size_t pos, int32_t value) { set_i32_be(bytes, pos, value, INT_BYTES); }

void BitUtil::set_i32_be(std::span<uint8_t> bytes, size_t pos, int32_t value, size_t len) {
    write_unaligned(bytes, pos, bswap(value), len);
}

int64_t BitUtil::get_i64_be(std::span<const uint8_t> bytes, size_t pos) {
    const uint64_t raw = read_unaligned<uint64_t>(bytes, pos);
    return static_cast<int64_t>(bswap(raw));
}

void BitUtil::set_i64_be(std::span<uint8_t> bytes, size_t pos, int64_t value) { set_i64_be(bytes, pos, value, LONG_BYTES); }

void BitUtil::set_i64_be(std::span<uint8_t> bytes, size_t pos, int64_t value, size_t len) {
    write_unaligned(bytes, pos, bswap(value), len);
}

// ---- little endian ----
int16_t BitUtil::get_i16_le(std::span<const uint8_t> bytes, size_t pos) { return read_unaligned<int16_t>(bytes, pos); }

void BitUtil::set_i16_le(std::span<uint8_t> bytes, size_t pos, int16_t value) { set_i16_le(bytes, pos, value, SHORT_BYTES); }

void BitUtil::set_i16_le(std::span<uint8_t> bytes, size_t pos, int16_t value, size_t len) {
    write_unaligned(bytes, pos, value, len);
}

int32_t BitUtil::get_i32_le(std::span<const uint8_t> bytes, size_t pos) { return read_unaligned<int32_t>(bytes, pos); }

void BitUtil::set_i32_le(std::span<uint8_t> bytes, size_t pos, int32_t value) { set_i32_le(bytes, pos, value, INT_BYTES); }

void BitUtil::set_i32_le(std::span<uint8_t> bytes, size_t pos, int32_t value, size_t len) {
    write_unaligned(bytes, pos, value, len);
}

int64_t BitUtil::get_i64_le(std::span<const uint8_t> bytes, size_t pos) { return read_unaligned<int64_t>(bytes, pos); }

void BitUtil::set_i64_le(std::span<uint8_t> bytes, size_t pos, int64_t value) { set_i64_le(bytes, pos, value, LONG_BYTES); }

void BitUtil::set_i64_le(std::span<uint8_t> bytes, size_t pos, int64_t value, size_t len) {
    write_unaligned(bytes, pos, value, len);
}

int32_t BitUtil::next_highest_power_of_two_with_i32(int32_t v) {
    v -= 1;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

int64_t BitUtil::next_highest_power_of_two_with_i64(int64_t v) {
    v -= 1;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

namespace {
constexpr uint64_t MAGIC0 = 0x5555'5555'5555'5555ull;
constexpr uint64_t MAGIC1 = 0x3333'3333'3333'3333ull;
constexpr uint64_t MAGIC2 = 0x0F0F'0F0F'0F0F'0F0Full;
constexpr uint64_t MAGIC3 = 0x00FF'00FF'00FF'00FFull;
constexpr uint64_t MAGIC4 = 0x0000'FFFF'0000'FFFFull;
constexpr uint64_t MAGIC5 = 0x0000'0000'FFFF'FFFFull;
constexpr uint64_t MAGIC6 = 0xAAAA'AAAA'AAAA'AAAAull;
} // namespace

int64_t BitUtil::interleave(int32_t even, int32_t odd) {
    uint64_t v1 = MAGIC5 & static_cast<uint64_t>(static_cast<uint32_t>(even));
    uint64_t v2 = MAGIC5 & static_cast<uint64_t>(static_cast<uint32_t>(odd));

    v1 = (v1 | (v1 << 16)) & MAGIC4;
    v1 = (v1 | (v1 << 8)) & MAGIC3;
    v1 = (v1 | (v1 << 4)) & MAGIC2;
    v1 = (v1 | (v1 << 2)) & MAGIC1;
    v1 = (v1 | (v1 << 1)) & MAGIC0;

    v2 = (v2 | (v2 << 16)) & MAGIC4;
    v2 = (v2 | (v2 << 8)) & MAGIC3;
    v2 = (v2 | (v2 << 4)) & MAGIC2;
    v2 = (v2 | (v2 << 2)) & MAGIC1;
    v2 = (v2 | (v2 << 1)) & MAGIC0;

    return static_cast<int64_t>((v2 << 1) | v1);
}

int64_t BitUtil::flip_flop(int64_t b) {
    const uint64_t value = static_cast<uint64_t>(b);
    return static_cast<int64_t>(((value & MAGIC6) >> 1) | ((value & MAGIC0) << 1));
}

int32_t BitUtil::zig_zag_decode_i32(uint32_t i) {
    return static_cast<int32_t>(i >> 1) ^ -static_cast<int32_t>(i & 1u);
}

int32_t BitUtil::zig_zag_encode_i32(int32_t i) { return (i >> 31) ^ (i << 1); }

int64_t BitUtil::zig_zag_decode_i64(uint64_t l) {
    return static_cast<int64_t>(l >> 1) ^ -static_cast<int64_t>(l & 1ull);
}

int64_t BitUtil::zig_zag_encode_i64(int64_t l) { return (l >> 63) ^ (l << 1); }

bool BitUtil::is_zero_or_power_of_two(int32_t x) {
    // `wrapping_sub` is required: 0 - 1 must wrap to -1 for the 0 case to work.
    return (x & static_cast<int32_t>(static_cast<uint32_t>(x) - 1u)) == 0;
}

} // namespace lucene
