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

module lucene_core:small_float.impl;

import :small_float;

import :error;
import :types;

import std;

namespace lucene {

namespace {

/// `f32::to_bits` reinterpreted as a signed integer, as the Rust code does.
int32_t float_bits_as_i32(float f) noexcept {
    return static_cast<int32_t>(std::bit_cast<uint32_t>(f));
}

} // namespace

uint8_t SmallFloat::float_to_byte(float f, int32_t num_mantissa_bits, int32_t zero_exp) noexcept {
    // Adjustment from a float zero exponent to our zero exponent, shifted over
    // to our exponent position.
    const int32_t fzero = (63 - zero_exp) << num_mantissa_bits;
    const int32_t bits = float_bits_as_i32(f);
    const int32_t smallfloat = bits >> (24 - num_mantissa_bits);
    if (smallfloat <= fzero) {
        return bits <= 0 ? uint8_t{0} : uint8_t{1};  // negative/zero map to 0, underflow to 1
    }
    if (smallfloat >= fzero + 0x100) {
        return uint8_t{255};  // overflow maps to the largest number
    }
    return static_cast<uint8_t>(smallfloat - fzero);
}

float SmallFloat::byte_to_float(uint8_t b, int32_t num_mantissa_bits, int32_t zero_exp) noexcept {
    if (b == 0) {
        return 0.0F;
    }
    int32_t bits = static_cast<int32_t>(b) << (24 - num_mantissa_bits);
    bits = static_cast<int32_t>(static_cast<uint32_t>(bits) +
                                (static_cast<uint32_t>(63 - zero_exp) << 24));
    return std::bit_cast<float>(static_cast<uint32_t>(bits));
}

uint8_t SmallFloat::float_to_byte_3_15(float f) noexcept {
    const int32_t bits = float_bits_as_i32(f);
    const int32_t smallfloat = bits >> (24 - 3);
    if (smallfloat <= ((63 - 15) << 3)) {
        return bits <= 0 ? uint8_t{0} : uint8_t{1};
    }
    if (smallfloat >= ((63 - 15) << 3) + 0x100) {
        return uint8_t{255};
    }
    return static_cast<uint8_t>(smallfloat - ((63 - 15) << 3));
}

float SmallFloat::byte_3_15_to_float(uint8_t b) noexcept {
    if (b == 0) {
        return 0.0F;
    }
    int32_t bits = static_cast<int32_t>(b) << (24 - 3);
    bits += (63 - 15) << 24;
    return std::bit_cast<float>(static_cast<uint32_t>(bits));
}

Result<int32_t> SmallFloat::long_to_int4(int64_t i) {
    if (i < 0) {
        return err(LuceneError::illegal_argument("Only supports positive values, got " + std::to_string(i)));
    }
    const int32_t num_bits = 64 - std::countl_zero(static_cast<uint64_t>(i));
    if (num_bits < 4) {
        return static_cast<int32_t>(i);
    }
    const int32_t shift = num_bits - 4;
    int64_t encoded = i >> shift;
    // only keep the 4 most significant bits
    encoded &= 0x07;
    // encode the shift, adding 1 because 0 is reserved for subnormal values
    encoded |= static_cast<int64_t>(shift + 1) << 3;
    if (encoded > std::numeric_limits<int32_t>::max()) {
        return err(LuceneError::illegal_argument("value does not fit in an int"));
    }
    return static_cast<int32_t>(encoded);
}

int64_t SmallFloat::int4_to_long(int32_t i) noexcept {
    const int64_t bits = i & 0x07;
    const int32_t shift = static_cast<int32_t>(static_cast<uint32_t>(i) >> 3) - 1;
    if (shift == -1) {
        // subnormal value
        return bits;
    }
    return (bits | 0x08) << shift;
}

Result<uint8_t> SmallFloat::int_to_byte4(int32_t i) {
    if (i < 0) {
        return err(LuceneError::illegal_argument("Only supports positive values, got " + std::to_string(i)));
    }
    if (i < NUM_FREE_VALUES) {
        return static_cast<uint8_t>(i);
    }
    auto encoded = long_to_int4(static_cast<int64_t>(i) - NUM_FREE_VALUES);
    if (!encoded.has_value()) {
        return std::unexpected(std::move(encoded.error()));
    }
    const int32_t value = NUM_FREE_VALUES + encoded.value();
    if (value > 255) {
        return err(LuceneError::illegal_argument("encoded value does not fit in a byte"));
    }
    return static_cast<uint8_t>(value);
}

Result<int32_t> SmallFloat::byte4_to_int(uint8_t b) {
    const int32_t i = b;
    if (i < NUM_FREE_VALUES) {
        return i;
    }
    const int64_t value = static_cast<int64_t>(NUM_FREE_VALUES) + int4_to_long(i - NUM_FREE_VALUES);
    if (value > std::numeric_limits<int32_t>::max()) {
        return err(LuceneError::illegal_argument("decoded value does not fit in an int"));
    }
    return static_cast<int32_t>(value);
}

} // namespace lucene
