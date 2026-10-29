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

module lucene_core:numeric_utils.impl;

import :numeric_utils;
import :types;
import :error;
import :debug;
import :bit_util;

import std;

namespace lucene {

int64_t NumericUtils::sortable_double_bits(int64_t bits) noexcept {
    return bits ^ ((bits >> 63) & 0x7fff'ffff'ffff'ffffLL);
}

int32_t NumericUtils::sortable_float_bits(int32_t bits) noexcept {
    return bits ^ ((bits >> 31) & 0x7fff'ffff);
}

int64_t NumericUtils::double_to_sortable_long(double value) noexcept {
    // Non-standard `NaN` payloads are normalized so that every `NaN` compares
    // equal, matching `f64::total_cmp` behaviour of the Rust port.
    const int64_t bits = std::isnan(value) ? static_cast<int64_t>(BitUtil::DOUBLE_NAN_BITS)
                                           : static_cast<int64_t>(std::bit_cast<uint64_t>(value));
    return sortable_double_bits(bits);
}

double NumericUtils::sortable_long_to_double(int64_t encoded) noexcept {
    return std::bit_cast<double>(static_cast<uint64_t>(sortable_double_bits(encoded)));
}

int32_t NumericUtils::float_to_sortable_int(float value) noexcept {
    const int32_t bits = std::isnan(value) ? static_cast<int32_t>(BitUtil::FLOAT_NAN_BITS)
                                           : std::bit_cast<int32_t>(value);
    return sortable_float_bits(bits);
}

float NumericUtils::sortable_int_to_float(int32_t encoded) noexcept {
    return std::bit_cast<float>(sortable_float_bits(encoded));
}

VoidResult NumericUtils::subtract(size_t bytes_per_dim, size_t dim, std::span<const uint8_t> a,
                                  std::span<const uint8_t> b, std::span<uint8_t> result) {
    const size_t start = dim * bytes_per_dim;
    const size_t end = start + bytes_per_dim;
    if (end > a.size() || end > b.size() || bytes_per_dim > result.size()) {
        return err(LuceneError::illegal_argument("dimension is out of bounds"));
    }
    int32_t borrow = 0;
    for (size_t i = end; i-- > start;) {
        const int32_t diff = static_cast<int32_t>(a[i]) - static_cast<int32_t>(b[i]) - borrow;
        if (diff < 0) {
            borrow = 1;
            result[i - start] = static_cast<uint8_t>(diff + 256);
        } else {
            borrow = 0;
            result[i - start] = static_cast<uint8_t>(diff);
        }
    }
    if (borrow != 0) {
        return err(LuceneError::illegal_argument("a < b"));
    }
    return {};
}

VoidResult NumericUtils::add(size_t bytes_per_dim, size_t dim, std::span<const uint8_t> a, std::span<const uint8_t> b,
                             std::span<uint8_t> result) {
    const size_t start = dim * bytes_per_dim;
    const size_t end = start + bytes_per_dim;
    if (end > a.size() || end > b.size() || bytes_per_dim > result.size()) {
        return err(LuceneError::illegal_argument("dimension is out of bounds"));
    }
    int32_t carry = 0;
    for (size_t i = end; i-- > start;) {
        const int32_t sum = static_cast<int32_t>(a[i]) + static_cast<int32_t>(b[i]) + carry;
        if (sum > 255) {
            carry = 1;
            result[i - start] = static_cast<uint8_t>(sum - 256);
        } else {
            carry = 0;
            result[i - start] = static_cast<uint8_t>(sum);
        }
    }
    if (carry != 0) {
        return err(LuceneError::illegal_argument("a + b overflows bytesPerDim=" + std::to_string(bytes_per_dim)));
    }
    return {};
}

void NumericUtils::int_to_sortable_bytes(int32_t value, std::span<uint8_t> result, size_t offset) {
    debug_check(offset + BitUtil::INT_BYTES <= result.size(), "offset out of bounds");
    // Flipping the sign bit turns the signed order into the unsigned one.
    BitUtil::set_i32_be(result, offset, value ^ std::numeric_limits<int32_t>::min());
}

int32_t NumericUtils::sortable_bytes_to_int(std::span<const uint8_t> encoded, size_t offset) {
    debug_check(offset + BitUtil::INT_BYTES <= encoded.size(), "offset out of bounds");
    return BitUtil::get_i32_be(encoded, offset) ^ std::numeric_limits<int32_t>::min();
}

void NumericUtils::long_to_sortable_bytes(int64_t value, std::span<uint8_t> result, size_t offset) {
    debug_check(offset + BitUtil::LONG_BYTES <= result.size(), "offset out of bounds");
    BitUtil::set_i64_be(result, offset, value ^ std::numeric_limits<int64_t>::min());
}

int64_t NumericUtils::sortable_bytes_to_long(std::span<const uint8_t> encoded, size_t offset) {
    debug_check(offset + BitUtil::LONG_BYTES <= encoded.size(), "offset out of bounds");
    return BitUtil::get_i64_be(encoded, offset) ^ std::numeric_limits<int64_t>::min();
}

void NumericUtils::int128_to_sortable_bytes(Int128 value, std::span<uint8_t> result, size_t offset) {
    debug_check(offset + 16 <= result.size(), "offset out of bounds");
    const unsigned __int128 bits =
        static_cast<unsigned __int128>(value) ^ (static_cast<unsigned __int128>(1) << 127);
    for (size_t i = 0; i < 16; ++i) {
        result[offset + i] = static_cast<uint8_t>(bits >> (8 * (15 - i)));
    }
}

Int128 NumericUtils::sortable_bytes_to_int128(std::span<const uint8_t> encoded, size_t offset) {
    debug_check(offset + 16 <= encoded.size(), "offset out of bounds");
    unsigned __int128 bits = 0;
    for (size_t i = 0; i < 16; ++i) {
        bits = (bits << 8) | static_cast<unsigned __int128>(encoded[offset + i]);
    }
    return static_cast<Int128>(bits ^ (static_cast<unsigned __int128>(1) << 127));
}

} // namespace lucene
