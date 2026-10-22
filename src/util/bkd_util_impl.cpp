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

module lucene_core:bkd_util.impl;

import :bkd_util;

import :bit_util;
import :core_helper;
import :debug;
import :types;

import std;

namespace lucene {

namespace {

/// Big-endian byte order turns "count leading zero bits" into "count leading
/// common bytes", which is exactly what the reference implementation's `swap_bytes` trick does: the
/// values are read little endian, so the *first* byte lives in the low bits and
/// must be moved to the top before counting.
/// (`<bit>` is not re-exported by `import std`, so the compiler builtins are
/// used instead of `std::byteswap` / `std::countl_zero`.)
template <typename T>
int32_t common_prefix_bytes(T left, T right) {
    using Unsigned = std::make_unsigned_t<T>;
    if constexpr (sizeof(T) == 8) {
        const auto difference = __builtin_bswap64(static_cast<uint64_t>(static_cast<Unsigned>(left ^ right)));
        return static_cast<int32_t>(difference == 0 ? 8 : (__builtin_clzll(difference) >> 3));
    } else {
        const auto difference = __builtin_bswap32(static_cast<uint32_t>(static_cast<Unsigned>(left ^ right)));
        return static_cast<int32_t>(difference == 0 ? 4 : (__builtin_clz(difference) >> 3));
    }
}

} // namespace

int32_t BKDUtil::common_prefix_length8(std::span<const uint8_t> a, size_t a_offset, std::span<const uint8_t> b,
                                       size_t b_offset) {
    debug_check(a_offset + 8 <= a.size() && b_offset + 8 <= b.size(), "8 byte prefix needs 8 readable bytes");
    return common_prefix_bytes<int64_t>(BitUtil::get_i64_le(a, a_offset), BitUtil::get_i64_le(b, b_offset));
}

int32_t BKDUtil::common_prefix_length4(std::span<const uint8_t> a, size_t a_offset, std::span<const uint8_t> b,
                                       size_t b_offset) {
    debug_check(a_offset + 4 <= a.size() && b_offset + 4 <= b.size(), "4 byte prefix needs 4 readable bytes");
    return common_prefix_bytes<int32_t>(BitUtil::get_i32_le(a, a_offset), BitUtil::get_i32_le(b, b_offset));
}

int32_t BKDUtil::common_prefix_length_n(std::span<const uint8_t> a, size_t a_offset, std::span<const uint8_t> b,
                                        size_t b_offset, size_t num_bytes) {
    debug_check(a_offset + num_bytes <= a.size() && b_offset + num_bytes <= b.size(),
                "the compared prefix must fit into both arrays");
    const int32_t mismatch =
        CoreHelper::mismatch_u8(a.subspan(a_offset, num_bytes), b.subspan(b_offset, num_bytes));
    return mismatch == -1 ? static_cast<int32_t>(num_bytes) : mismatch;
}

bool BKDUtil::equals8(std::span<const uint8_t> a, size_t a_offset, std::span<const uint8_t> b, size_t b_offset) {
    debug_check(a_offset + 8 <= a.size() && b_offset + 8 <= b.size(), "8 byte comparison needs 8 readable bytes");
    return BitUtil::get_i64_le(a, a_offset) == BitUtil::get_i64_le(b, b_offset);
}

bool BKDUtil::equals4(std::span<const uint8_t> a, size_t a_offset, std::span<const uint8_t> b, size_t b_offset) {
    debug_check(a_offset + 4 <= a.size() && b_offset + 4 <= b.size(), "4 byte comparison needs 4 readable bytes");
    return BitUtil::get_i32_le(a, a_offset) == BitUtil::get_i32_le(b, b_offset);
}

int32_t BKDUtil::compare_unsigned(std::span<const uint8_t> a, size_t a_offset, std::span<const uint8_t> b,
                                  size_t b_offset, size_t num_bytes) {
    debug_check(a_offset + num_bytes <= a.size() && b_offset + num_bytes <= b.size(),
                "the compared bytes must fit into both arrays");
    const int32_t mismatch =
        CoreHelper::mismatch_u8(a.subspan(a_offset, num_bytes), b.subspan(b_offset, num_bytes));
    if (mismatch == -1) {
        return 0;
    }
    const uint8_t left = a[a_offset + static_cast<size_t>(mismatch)];
    const uint8_t right = b[b_offset + static_cast<size_t>(mismatch)];
    return left < right ? -1 : 1;
}

bool ByteArrayPredicate::test(std::span<const uint8_t> a, size_t a_offset, std::span<const uint8_t> b,
                              size_t b_offset) const {
    if (num_bytes == BitUtil::LONG_BYTES) {
        return BKDUtil::equals8(a, a_offset, b, b_offset);
    }
    if (num_bytes == BitUtil::INT_BYTES) {
        return BKDUtil::equals4(a, a_offset, b, b_offset);
    }
    debug_check(a_offset + num_bytes <= a.size() && b_offset + num_bytes <= b.size(),
                "the compared bytes must fit into both arrays");
    return std::equal(a.begin() + static_cast<std::ptrdiff_t>(a_offset),
                      a.begin() + static_cast<std::ptrdiff_t>(a_offset + num_bytes),
                      b.begin() + static_cast<std::ptrdiff_t>(b_offset));
}

int32_t ByteArrayComparator::common_prefix_length(std::span<const uint8_t> a, size_t a_offset,
                                                  std::span<const uint8_t> b, size_t b_offset) const {
    if (num_bytes == BitUtil::LONG_BYTES) {
        return BKDUtil::common_prefix_length8(a, a_offset, b, b_offset);
    }
    if (num_bytes == BitUtil::INT_BYTES) {
        return BKDUtil::common_prefix_length4(a, a_offset, b, b_offset);
    }
    return BKDUtil::common_prefix_length_n(a, a_offset, b, b_offset, num_bytes);
}

} // namespace lucene
