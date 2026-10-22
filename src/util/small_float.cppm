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

export module lucene_core:small_float;

import :types;
import std;

import :error;

namespace lucene {

/// Floating-point-like encodings into a single byte (or a small int).
///
/// Port of `SmallFloat` (`core/util/small_float.rs`).  Two families are
/// implemented:
///
///  * a generic 8-bit float (`float_to_byte` / `byte_to_float`) with a
///    configurable mantissa width and zero exponent, plus the `3/15` preset that
///    Lucene stores in some formats;
///  * `int_to_byte4` / `byte4_to_int`, an ordering-preserving encoding of
///    non-negative `int32_t` with 4 significant bits, used for length norms.
export class SmallFloat {
public:
    /// Number of small values `int_to_byte4` keeps as-is (`255 - MAX_INT4`).
    static constexpr int32_t NUM_FREE_VALUES = 255 - (((32 - 4) << 3) | 0x07);

    /// Encodes `f` as an 8-bit float with `num_mantissa_bits` mantissa bits and
    /// `zero_exp` as the exponent of zero.
    ///
    /// Negative values map to `0`, values are truncated towards zero (except
    /// that an underflow maps to `1`, the smallest non-zero value) and an
    /// overflow maps to `255`.
    [[nodiscard]] static uint8_t float_to_byte(float f, int32_t num_mantissa_bits, int32_t zero_exp) noexcept;

    /// Decodes a byte written by [`float_to_byte`].
    [[nodiscard]] static float byte_to_float(uint8_t b, int32_t num_mantissa_bits, int32_t zero_exp) noexcept;

    /// `float_to_byte(f, 3, 15)`: 3 mantissa bits, smallest non-zero value
    /// `5.820766E-10`, largest `7.5161928E9`, epsilon `0.125`.
    [[nodiscard]] static uint8_t float_to_byte_3_15(float f) noexcept;
    /// `byte_to_float(b, 3, 15)`.
    [[nodiscard]] static float byte_3_15_to_float(uint8_t b) noexcept;

    /// Encodes a non-negative `int64_t` into 4 significant bits plus an
    /// exponent; ordering is preserved.
    [[nodiscard]] static Result<int32_t> long_to_int4(int64_t i);

    /// Decodes a value written by [`long_to_int4`].
    [[nodiscard]] static int64_t int4_to_long(int32_t i) noexcept;

    /// Encodes a non-negative `int32_t` into a byte; values below
    /// [`NUM_FREE_VALUES`] are stored verbatim.
    [[nodiscard]] static Result<uint8_t> int_to_byte4(int32_t i);

    /// Decodes a byte written by [`int_to_byte4`].
    [[nodiscard]] static Result<int32_t> byte4_to_int(uint8_t b);
};

} // namespace lucene
