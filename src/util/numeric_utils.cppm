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

export module lucene_core:numeric_utils;

import :types;
import :error;

import std;

namespace lucene {

/// Conversions between Java's numeric types and their sortable encodings.
///
/// Port of `util/numeric_utils.rs`.  Everything here is byte-for-byte identical
/// to Lucene's `NumericUtils`, which is what makes lucene99 index files
/// interchangeable.
///
/// **Deviation:** the `BigInteger` conversions (`big_int_to_sortable_bytes` /
/// `sortable_bytes_to_big_int`) are not ported: C++ has no arbitrary precision
/// integer, and no codec in Lucene 9.x stores big integers in a sortable byte
/// form (only `BigIntegerPoint`, which is out of scope so far).
export struct NumericUtils {
    // ---- floating point <-> sortable integers ----

    /// Converts a `double` to a sortable `int64_t`; `NaN` maps to a canonical
    /// value that sorts above positive infinity.
    [[nodiscard]] static int64_t double_to_sortable_long(double value) noexcept;

    /// Inverse of [`double_to_sortable_long`].
    [[nodiscard]] static double sortable_long_to_double(int64_t encoded) noexcept;

    /// Converts a `float` to a sortable `int32_t`; `NaN` maps to a canonical
    /// value that sorts above positive infinity.
    [[nodiscard]] static int32_t float_to_sortable_int(float value) noexcept;

    /// Inverse of [`float_to_sortable_int`].
    [[nodiscard]] static float sortable_int_to_float(int32_t encoded) noexcept;

    /// Converts the IEEE 754 layout of a `double` to sortable order (or back).
    [[nodiscard]] static int64_t sortable_double_bits(int64_t bits) noexcept;

    /// Converts the IEEE 754 layout of a `float` to sortable order (or back).
    [[nodiscard]] static int32_t sortable_float_bits(int32_t bits) noexcept;

    // ---- byte-wise arithmetic on point dimensions ----

    /// `result = a - b` for the `dim`-th dimension; fails when `a < b`.
    [[nodiscard]] static VoidResult subtract(size_t bytes_per_dim, size_t dim, std::span<const uint8_t> a,
                                             std::span<const uint8_t> b, std::span<uint8_t> result);

    /// `result = a + b` (unsigned) for the `dim`-th dimension; fails on overflow.
    [[nodiscard]] static VoidResult add(size_t bytes_per_dim, size_t dim, std::span<const uint8_t> a,
                                        std::span<const uint8_t> b, std::span<uint8_t> result);

    // ---- integers <-> lexicographically sortable bytes ----

    /// Writes `value` as 4 big-endian bytes with the sign bit flipped.
    static void int_to_sortable_bytes(int32_t value, std::span<uint8_t> result, size_t offset);

    /// Reads a value written by [`int_to_sortable_bytes`].
    [[nodiscard]] static int32_t sortable_bytes_to_int(std::span<const uint8_t> encoded, size_t offset);

    /// Writes `value` as 8 big-endian bytes with the sign bit flipped.
    static void long_to_sortable_bytes(int64_t value, std::span<uint8_t> result, size_t offset);

    /// Reads a value written by [`long_to_sortable_bytes`].
    [[nodiscard]] static int64_t sortable_bytes_to_long(std::span<const uint8_t> encoded, size_t offset);
};

} // namespace lucene
