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

export module clucene:bit_util;

import :types;
import std;

namespace clucene {

/// Port of `core/util/bit_util.rs`.
///
/// All the accessors take a `std::span` instead of the Rust `&[u8]` / `&mut [u8]`
/// pair; bounds are asserted in debug builds exactly like the Rust
/// `debug_assert!`s.
export struct BitUtil {
    static constexpr size_t BYTE_BYTES = sizeof(int8_t);
    static constexpr size_t SHORT_BYTES = sizeof(int16_t);
    static constexpr size_t INT_BYTES = sizeof(int32_t);
    static constexpr size_t LONG_BYTES = sizeof(int64_t);
    static constexpr size_t FLOAT_BYTES = sizeof(float);
    static constexpr size_t DOUBLE_BYTES = sizeof(double);
    static constexpr size_t USIZE_BYTES = sizeof(size_t);

    /// The smallest positive subnormal float value.
    static constexpr float F32_MIN_VALUE = std::bit_cast<float>(static_cast<uint32_t>(1));
    /// The smallest positive subnormal double value.
    static constexpr double F64_MIN_VALUE = std::bit_cast<double>(static_cast<uint64_t>(1));
    static constexpr uint32_t FLOAT_NAN_BITS = 0x7fc00000u;
    static constexpr uint64_t DOUBLE_NAN_BITS = 0x7ff8000000000000ull;

    /// Returns float bits with all NaN representations canonicalized.
    [[nodiscard]] static int32_t float_to_int_bits(float value);
    /// Returns double bits with all NaN representations canonicalized.
    [[nodiscard]] static int64_t double_to_long_bits(double value);

    // ---- big endian ----
    [[nodiscard]] static int16_t get_i16_be(std::span<const uint8_t> bytes, size_t pos);
    static void set_i16_be(std::span<uint8_t> bytes, size_t pos, int16_t value);
    static void set_i16_be(std::span<uint8_t> bytes, size_t pos, int16_t value, size_t len);

    [[nodiscard]] static int32_t get_i32_be(std::span<const uint8_t> bytes, size_t pos);
    static void set_i32_be(std::span<uint8_t> bytes, size_t pos, int32_t value);
    static void set_i32_be(std::span<uint8_t> bytes, size_t pos, int32_t value, size_t len);

    [[nodiscard]] static int64_t get_i64_be(std::span<const uint8_t> bytes, size_t pos);
    static void set_i64_be(std::span<uint8_t> bytes, size_t pos, int64_t value);
    static void set_i64_be(std::span<uint8_t> bytes, size_t pos, int64_t value, size_t len);

    // ---- little endian (host order on every supported target) ----
    [[nodiscard]] static int16_t get_i16_le(std::span<const uint8_t> bytes, size_t pos);
    static void set_i16_le(std::span<uint8_t> bytes, size_t pos, int16_t value);
    static void set_i16_le(std::span<uint8_t> bytes, size_t pos, int16_t value, size_t len);

    [[nodiscard]] static int32_t get_i32_le(std::span<const uint8_t> bytes, size_t pos);
    static void set_i32_le(std::span<uint8_t> bytes, size_t pos, int32_t value);
    static void set_i32_le(std::span<uint8_t> bytes, size_t pos, int32_t value, size_t len);

    [[nodiscard]] static int64_t get_i64_le(std::span<const uint8_t> bytes, size_t pos);
    static void set_i64_le(std::span<uint8_t> bytes, size_t pos, int64_t value);
    static void set_i64_le(std::span<uint8_t> bytes, size_t pos, int64_t value, size_t len);

    /// Returns the next highest power of two, or the current value if it is
    /// already a power of two or zero.
    [[nodiscard]] static int32_t next_highest_power_of_two_with_i32(int32_t v);
    /// Returns the next highest power of two, or the current value if it is
    /// already a power of two or zero.
    [[nodiscard]] static int64_t next_highest_power_of_two_with_i64(int64_t v);

    /// Interleaves the low 32 bits of each value.
    [[nodiscard]] static int64_t interleave(int32_t even, int32_t odd);
    /// Swaps odd and even bits.
    [[nodiscard]] static int64_t flip_flop(int64_t b);

    [[nodiscard]] static int32_t zig_zag_decode_i32(uint32_t i);
    [[nodiscard]] static int32_t zig_zag_encode_i32(int32_t i);
    [[nodiscard]] static int64_t zig_zag_decode_i64(uint64_t l);
    [[nodiscard]] static int64_t zig_zag_encode_i64(int64_t l);

    /// True if, and only if, the provided integer - treated as unsigned - is
    /// either 0 or a power of two.
    [[nodiscard]] static bool is_zero_or_power_of_two(int32_t x);
};

static_assert(std::endian::native == std::endian::little,
              "clucene requires a little-endian host (rlucene has the same constraint)");

} // namespace clucene
