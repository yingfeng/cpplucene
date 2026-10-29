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

export module lucene_core:half_float_point;

import :indexable_field;
import :query;
import :types;

import std;

namespace lucene {

/// An indexed `half-float` point field, for fast range filters.
///
/// Port of `HalfFloatPoint` (`sandbox/document/`).  Values are given as `float`
/// and encoded to half floats when indexed; a value that cannot be represented
/// exactly is rounded to the closest half float (ties round to even).  The
/// encodings are the sortable ones, so byte order equals numeric order.
///
/// Like the other point fields of this port, the factory returns a plain
/// [`Field`] and the encoders are static functions.
export struct HalfFloatPoint {
    /// The number of bytes one dimension takes.
    static constexpr size_t BYTES = 2;

    // ---- half-float conversion ----

    /// The first half float strictly greater than `v`.
    ///
    /// `NaN` and `+inf` are returned unchanged.
    [[nodiscard]] static float next_up(float v);
    /// The first half float strictly smaller than `v`.
    ///
    /// `NaN` and `-inf` are returned unchanged.
    [[nodiscard]] static float next_down(float v);

    /// `v` as the sortable short encoding.
    [[nodiscard]] static int16_t half_float_to_sortable_short(float v);
    /// The inverse of [`half_float_to_sortable_short`](HalfFloatPoint::half_float_to_sortable_short).
    [[nodiscard]] static float sortable_short_to_half_float(int16_t bits);

    /// `v` as half-float bits (not sortable).
    [[nodiscard]] static int16_t half_float_to_short_bits(float v);
    /// The inverse of [`half_float_to_short_bits`](HalfFloatPoint::half_float_to_short_bits).
    [[nodiscard]] static float short_bits_to_half_float(int16_t bits);

    /// Writes the big-endian sortable form of `value` at `result[offset]`.
    static void short_to_sortable_bytes(int16_t value, std::span<uint8_t> result, size_t offset);
    /// Reads a big-endian sortable short written by
    /// [`short_to_sortable_bytes`](HalfFloatPoint::short_to_sortable_bytes).
    [[nodiscard]] static int16_t sortable_bytes_to_short(std::span<const uint8_t> encoded, size_t offset);

    /// Encodes one dimension of a point.
    static void encode_dimension(float value, std::span<uint8_t> dest, size_t offset);
    /// Decodes one dimension of a point.
    [[nodiscard]] static float decode_dimension(std::span<const uint8_t> value, size_t offset);

    // ---- fields and queries ----

    /// Indexes the point; one to eight dimensions.
    [[nodiscard]] static Result<Field> create(std::string name, std::span<const float> point);
    /// The packed form of `point`, as the point index stores it.
    [[nodiscard]] static Result<std::vector<uint8_t>> pack(std::span<const float> point);

    /// Matches the documents whose point is exactly `value` (one dimension).
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_exact_query(std::string field, float value);
    /// Matches the documents whose point is one of `values` (one dimension).
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_set_query(std::string field,
                                                                     std::span<const float> values);
    /// Matches the documents whose point lies in `[lower_value, upper_value]` (one
    /// dimension).
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query(std::string field, float lower_value,
                                                                       float upper_value);
    /// The N-dimensional range query.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query_n(std::string field,
                                                                         std::span<const float> lower_value,
                                                                         std::span<const float> upper_value);

private:
    /// The self-inverse bit flip that makes the half-float bits sort like the values.
    [[nodiscard]] static int16_t sortable_short_bits(int16_t bits);
};

} // namespace lucene
