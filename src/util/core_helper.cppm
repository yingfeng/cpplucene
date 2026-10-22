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

export module lucene_core:core_helper;

import :types;
import std;

import :error;

namespace lucene {

/// Port of the range-check / comparison helpers of `core/util/core_helper.rs`.
export struct CoreHelper {
    /// Checks that `[from_index, from_index + size)` fits into `length` and
    /// returns `from_index`, exactly like the Rust helper.
    [[nodiscard]] static Result<size_t> check_from_index_size(size_t from_index, size_t size, size_t length);

    /// Checks `from_index <= to_index <= length`.
    [[nodiscard]] static Result<size_t> check_from_to_index(size_t from_index, size_t to_index, size_t length);

    /// Checks `0 <= index < length`.
    [[nodiscard]] static Result<size_t> check_index(size_t index, size_t length);

    /// Returns the index of the first mismatch between `a` and `b`, or `-1` when
    /// both slices are fully equal.  A differing length alone counts as a
    /// mismatch at `min(a.size(), b.size())`.
    [[nodiscard]] static int32_t mismatch_u8(std::span<const uint8_t> a, std::span<const uint8_t> b);

    /// `int32_t` flavour of [`mismatch_u8`].
    [[nodiscard]] static int32_t mismatch_i32(std::span<const int32_t> a, std::span<const int32_t> b);

    // ---- float/double comparison matching Java's Float/Double.compare ----

    [[nodiscard]] static int32_t compare_f32(float a, float b);
    [[nodiscard]] static int32_t compare_f64(double a, double b);

    [[nodiscard]] static float max_f32(float a, float b);
    [[nodiscard]] static float min_f32(float a, float b);
    [[nodiscard]] static double max_f64(double a, double b);
    [[nodiscard]] static double min_f64(double a, double b);
};

} // namespace lucene
