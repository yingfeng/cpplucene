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

export module lucene_core:point_fields;

import :types;
import std;

import :error;
import :indexable_field;
import :query;

namespace lucene {

/// An indexed N-dimensional integer point field, for fast range filters.
///
/// Port of `IntPoint` (`core/document/int_point.rs`).
///
/// **Deviation:** the reference implementation wraps a mutable field object with
/// `set_int_values`; the port's field factories return a plain [`Field`] (change the
/// value with [`Field::set_bytes_value`]) and expose the encoders as free static
/// functions.
export struct IntPoint {
    static constexpr size_t BYTES = sizeof(int32_t);

    /// Indexes the point; one to eight dimensions.
    [[nodiscard]] static Result<Field> create(std::string name, std::span<const int32_t> point);

    /// The packed form of `point`, as the point index stores it.
    [[nodiscard]] static Result<std::vector<uint8_t>> pack(std::span<const int32_t> point);

    // ---- queries ----

    /// Matches the documents whose point is exactly `value` (one dimension).
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_exact_query(std::string field, int32_t value);
    /// Matches the documents whose point is one of `values` (one dimension).
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_set_query(std::string field,
                                                                      std::span<const int32_t> values);
    /// Matches the documents whose point lies in `[lower_value, upper_value]` (one
    /// dimension).
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query(std::string field, int32_t lower_value,
                                                                        int32_t upper_value);
    /// The N-dimensional range query.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query_n(std::string field,
                                                                          std::span<const int32_t> lower_value,
                                                                          std::span<const int32_t> upper_value);
};

/// The 64-bit sibling of [`IntPoint`].  Port of `LongPoint`.
export struct LongPoint {
    static constexpr size_t BYTES = sizeof(int64_t);

    [[nodiscard]] static Result<Field> create(std::string name, std::span<const int64_t> point);
    [[nodiscard]] static Result<std::vector<uint8_t>> pack(std::span<const int64_t> point);

    [[nodiscard]] static Result<std::shared_ptr<Query>> new_exact_query(std::string field, int64_t value);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_set_query(std::string field,
                                                                      std::span<const int64_t> values);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query(std::string field, int64_t lower_value,
                                                                        int64_t upper_value);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query_n(std::string field,
                                                                          std::span<const int64_t> lower_value,
                                                                          std::span<const int64_t> upper_value);
};

/// A float point field.  Port of `FloatPoint`.
export struct FloatPoint {
    static constexpr size_t BYTES = sizeof(float);

    [[nodiscard]] static Result<Field> create(std::string name, std::span<const float> point);
    [[nodiscard]] static Result<std::vector<uint8_t>> pack(std::span<const float> point);

    [[nodiscard]] static Result<std::shared_ptr<Query>> new_exact_query(std::string field, float value);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_set_query(std::string field,
                                                                      std::span<const float> values);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query(std::string field, float lower_value,
                                                                        float upper_value);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query_n(std::string field,
                                                                          std::span<const float> lower_value,
                                                                          std::span<const float> upper_value);
};

/// A double point field.  Port of `DoublePoint`.
export struct DoublePoint {
    static constexpr size_t BYTES = sizeof(double);

    [[nodiscard]] static Result<Field> create(std::string name, std::span<const double> point);
    [[nodiscard]] static Result<std::vector<uint8_t>> pack(std::span<const double> point);

    [[nodiscard]] static Result<std::shared_ptr<Query>> new_exact_query(std::string field, double value);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_set_query(std::string field,
                                                                      std::span<const double> values);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query(std::string field, double lower_value,
                                                                        double upper_value);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query_n(std::string field,
                                                                          std::span<const double> lower_value,
                                                                          std::span<const double> upper_value);
};

/// A point field over opaque byte dimensions of 1 to 8 bytes each.
///
/// Port of `BinaryPoint` (`core/document/binary_point.rs`).
export struct BinaryPoint {
    /// Every dimension of `point` must have the same length, 1 to 8 bytes.
    [[nodiscard]] static Result<Field> create(std::string name,
                                              std::span<const std::vector<uint8_t>> point);

    [[nodiscard]] static Result<std::shared_ptr<Query>> new_exact_query(std::string field,
                                                                        std::span<const uint8_t> value);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_set_query(
            std::string field, std::span<const std::vector<uint8_t>> values);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query(std::string field,
                                                                        std::span<const uint8_t> lower_value,
                                                                        std::span<const uint8_t> upper_value);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query_n(
            std::string field, std::span<const std::vector<uint8_t>> lower_value,
            std::span<const std::vector<uint8_t>> upper_value);
};

} // namespace lucene
