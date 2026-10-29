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

export module lucene_core:big_integer_point;

import :indexable_field;
import :numeric_utils;
import :query;
import :types;

import std;

namespace lucene {

/// An indexed 128-bit integer point field, for fast range filters.
///
/// Port of `BigIntegerPoint` (`sandbox/document/`).  The field stores 16 bytes
/// per dimension in the sortable encoding, so byte order equals numeric order.
///
/// **Deviations:**
///  * Java and the reference implementation take an arbitrary precision
///    `BigInteger`; the port takes [`Int128`], which is exactly the range the
///    field can index.  [`parse`](BigIntegerPoint::parse) turns a decimal literal
///    into one and reports an overflow for values that do not fit.
///  * like the other point fields of this port, the factory returns a plain
///    [`Field`] and the encoders are static functions.
export struct BigIntegerPoint {
    /// The number of bytes one dimension takes.
    static constexpr size_t BYTES = 16;

    /// `-2^127`, the smallest value the field can index.
    [[nodiscard]] static Int128 min_value() noexcept;
    /// `2^127 - 1`, the largest value the field can index.
    [[nodiscard]] static Int128 max_value() noexcept;

    /// Indexes the point; one to eight dimensions.
    [[nodiscard]] static Result<Field> create(std::string name, std::span<const Int128> point);
    /// The packed form of `point`, as the point index stores it.
    [[nodiscard]] static Result<std::vector<uint8_t>> pack(std::span<const Int128> point);

    /// Encodes one dimension of a point.
    static void encode_dimension(Int128 value, std::span<uint8_t> dest, size_t offset);
    /// Decodes one dimension of a point.
    [[nodiscard]] static Int128 decode_dimension(std::span<const uint8_t> value, size_t offset);

    /// Parses a decimal literal; fails with `kNumberOverflow` when it does not fit
    /// in 128 bits and with `kNumberFormat` when it is not a number.
    [[nodiscard]] static Result<Int128> parse(std::string_view decimal);
    /// The decimal form of `value`.
    [[nodiscard]] static std::string to_string(Int128 value);

    // ---- queries ----

    /// Matches the documents whose point is exactly `value` (one dimension).
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_exact_query(std::string field, Int128 value);
    /// Matches the documents whose point is one of `values` (one dimension).
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_set_query(std::string field,
                                                                     std::span<const Int128> values);
    /// Matches the documents whose point lies in `[lower_value, upper_value]` (one
    /// dimension).
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query(std::string field, Int128 lower_value,
                                                                       Int128 upper_value);
    /// The N-dimensional range query.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query_n(std::string field,
                                                                         std::span<const Int128> lower_value,
                                                                         std::span<const Int128> upper_value);
};

} // namespace lucene
