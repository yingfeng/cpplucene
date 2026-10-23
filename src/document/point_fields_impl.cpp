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

module lucene_core:point_fields.impl;

import :point_fields;

import std;

import :error;
import :field_type;
import :indexable_field;
import :numeric_utils;
import :point_in_set_query;
import :point_range_query;
import :query;
import :types;

namespace lucene {

namespace {

constexpr size_t kMaxPointDimensions = 8;

/// Packs `values` into `values.size() * width` bytes with `encode`.
template <typename T, typename Encode>
[[nodiscard]] Result<std::vector<uint8_t>> pack_values(std::span<const T> values, size_t width, Encode encode) {
    if (values.empty()) {
        return err(LuceneError::illegal_argument("point must contain at least 1 value"));
    }
    if (values.size() > kMaxPointDimensions) {
        return err(LuceneError::illegal_argument("point must contain at most 8 values"));
    }
    std::vector<uint8_t> packed(values.size() * width, 0);
    for (size_t dim = 0; dim < values.size(); ++dim) {
        encode(values[dim], packed, dim * width);
    }
    return packed;
}

/// A point field of `dimensions` dimensions of `width` bytes each.
[[nodiscard]] Result<Field> point_field(std::string name, std::vector<uint8_t> packed, size_t dimensions,
                                        size_t width) {
    FieldType type;
    if (auto set = type.set_dimensions(dimensions, width); !set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }
    type.freeze();
    return Field::from_bytes(std::move(name), std::move(packed), std::move(type));
}

[[nodiscard]] Result<std::shared_ptr<Query>> range_query(std::string field, std::vector<uint8_t> lower_point,
                                                        std::vector<uint8_t> upper_point, size_t num_dims) {
    auto query = PointRangeQuery::create(std::move(field), std::move(lower_point), std::move(upper_point), num_dims);
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<PointRangeQuery>(std::move(query.value())));
}

[[nodiscard]] Result<std::shared_ptr<Query>> set_query(std::string field, std::vector<std::vector<uint8_t>> points,
                                                       size_t num_dims) {
    auto query = PointInSetQuery::create(std::move(field), std::move(points), num_dims);
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<PointInSetQuery>(std::move(query.value())));
}

/// `[value, value]` as the two endpoints of a one-dimension range query.
template <typename T>
[[nodiscard]] Result<std::shared_ptr<Query>> exact_query(std::string field, std::span<const T> value,
                                                         size_t width, auto encode) {
    auto packed = pack_values<T>(value, width, encode);
    if (!packed.has_value()) {
        return std::unexpected(std::move(packed.error()));
    }
    return range_query(std::move(field), packed.value(), std::move(packed.value()), 1);
}

} // namespace

// ---- IntPoint ----

Result<std::vector<uint8_t>> IntPoint::pack(std::span<const int32_t> point) {
    return pack_values<int32_t>(point, BYTES, [](int32_t value, std::span<uint8_t> bytes, size_t offset) {
        NumericUtils::int_to_sortable_bytes(value, bytes, offset);
    });
}

Result<Field> IntPoint::create(std::string name, std::span<const int32_t> point) {
    auto packed = pack(point);
    if (!packed.has_value()) {
        return std::unexpected(std::move(packed.error()));
    }
    return point_field(std::move(name), std::move(packed.value()), point.size(), BYTES);
}

Result<std::shared_ptr<Query>> IntPoint::new_exact_query(std::string field, int32_t value) {
    const std::array<int32_t, 1> point{value};
    return exact_query<int32_t>(std::move(field), point, BYTES,
                                [](int32_t v, std::span<uint8_t> bytes, size_t offset) {
                                    NumericUtils::int_to_sortable_bytes(v, bytes, offset);
                                });
}

Result<std::shared_ptr<Query>> IntPoint::new_set_query(std::string field, std::span<const int32_t> values) {
    if (values.empty()) {
        return err(LuceneError::illegal_argument("values must contain at least 1 value"));
    }
    std::vector<std::vector<uint8_t>> points;
    points.reserve(values.size());
    for (const int32_t value : values) {
        const std::array<int32_t, 1> point{value};
        auto packed = pack(point);
        if (!packed.has_value()) {
            return std::unexpected(std::move(packed.error()));
        }
        points.push_back(std::move(packed.value()));
    }
    return set_query(std::move(field), std::move(points), 1);
}

Result<std::shared_ptr<Query>> IntPoint::new_range_query(std::string field, int32_t lower_value,
                                                         int32_t upper_value) {
    const std::array<int32_t, 1> lower{lower_value};
    const std::array<int32_t, 1> upper{upper_value};
    return new_range_query_n(std::move(field), lower, upper);
}

Result<std::shared_ptr<Query>> IntPoint::new_range_query_n(std::string field, std::span<const int32_t> lower_value,
                                                           std::span<const int32_t> upper_value) {
    if (lower_value.size() != upper_value.size()) {
        return err(LuceneError::illegal_argument("dimension size must be equals to the number of lower dimensions"));
    }
    auto lower = pack(lower_value);
    if (!lower.has_value()) {
        return std::unexpected(std::move(lower.error()));
    }
    auto upper = pack(upper_value);
    if (!upper.has_value()) {
        return std::unexpected(std::move(upper.error()));
    }
    return range_query(std::move(field), std::move(lower.value()), std::move(upper.value()), lower_value.size());
}

// ---- LongPoint ----

Result<std::vector<uint8_t>> LongPoint::pack(std::span<const int64_t> point) {
    return pack_values<int64_t>(point, BYTES, [](int64_t value, std::span<uint8_t> bytes, size_t offset) {
        NumericUtils::long_to_sortable_bytes(value, bytes, offset);
    });
}

Result<Field> LongPoint::create(std::string name, std::span<const int64_t> point) {
    auto packed = pack(point);
    if (!packed.has_value()) {
        return std::unexpected(std::move(packed.error()));
    }
    return point_field(std::move(name), std::move(packed.value()), point.size(), BYTES);
}

Result<std::shared_ptr<Query>> LongPoint::new_exact_query(std::string field, int64_t value) {
    const std::array<int64_t, 1> point{value};
    return exact_query<int64_t>(std::move(field), point, BYTES,
                                [](int64_t v, std::span<uint8_t> bytes, size_t offset) {
                                    NumericUtils::long_to_sortable_bytes(v, bytes, offset);
                                });
}

Result<std::shared_ptr<Query>> LongPoint::new_set_query(std::string field, std::span<const int64_t> values) {
    if (values.empty()) {
        return err(LuceneError::illegal_argument("values must contain at least 1 value"));
    }
    std::vector<std::vector<uint8_t>> points;
    points.reserve(values.size());
    for (const int64_t value : values) {
        const std::array<int64_t, 1> point{value};
        auto packed = pack(point);
        if (!packed.has_value()) {
            return std::unexpected(std::move(packed.error()));
        }
        points.push_back(std::move(packed.value()));
    }
    return set_query(std::move(field), std::move(points), 1);
}

Result<std::shared_ptr<Query>> LongPoint::new_range_query(std::string field, int64_t lower_value,
                                                          int64_t upper_value) {
    const std::array<int64_t, 1> lower{lower_value};
    const std::array<int64_t, 1> upper{upper_value};
    return new_range_query_n(std::move(field), lower, upper);
}

Result<std::shared_ptr<Query>> LongPoint::new_range_query_n(std::string field, std::span<const int64_t> lower_value,
                                                            std::span<const int64_t> upper_value) {
    if (lower_value.size() != upper_value.size()) {
        return err(LuceneError::illegal_argument("dimension size must be equals to the number of lower dimensions"));
    }
    auto lower = pack(lower_value);
    if (!lower.has_value()) {
        return std::unexpected(std::move(lower.error()));
    }
    auto upper = pack(upper_value);
    if (!upper.has_value()) {
        return std::unexpected(std::move(upper.error()));
    }
    return range_query(std::move(field), std::move(lower.value()), std::move(upper.value()), lower_value.size());
}

// ---- FloatPoint ----

Result<std::vector<uint8_t>> FloatPoint::pack(std::span<const float> point) {
    return pack_values<float>(point, BYTES, [](float value, std::span<uint8_t> bytes, size_t offset) {
        NumericUtils::int_to_sortable_bytes(NumericUtils::float_to_sortable_int(value), bytes, offset);
    });
}

Result<Field> FloatPoint::create(std::string name, std::span<const float> point) {
    auto packed = pack(point);
    if (!packed.has_value()) {
        return std::unexpected(std::move(packed.error()));
    }
    return point_field(std::move(name), std::move(packed.value()), point.size(), BYTES);
}

Result<std::shared_ptr<Query>> FloatPoint::new_exact_query(std::string field, float value) {
    const std::array<float, 1> point{value};
    return exact_query<float>(std::move(field), point, BYTES,
                              [](float v, std::span<uint8_t> bytes, size_t offset) {
                                  NumericUtils::int_to_sortable_bytes(NumericUtils::float_to_sortable_int(v), bytes,
                                                                      offset);
                              });
}

Result<std::shared_ptr<Query>> FloatPoint::new_set_query(std::string field, std::span<const float> values) {
    if (values.empty()) {
        return err(LuceneError::illegal_argument("values must contain at least 1 value"));
    }
    std::vector<std::vector<uint8_t>> points;
    points.reserve(values.size());
    for (const float value : values) {
        const std::array<float, 1> point{value};
        auto packed = pack(point);
        if (!packed.has_value()) {
            return std::unexpected(std::move(packed.error()));
        }
        points.push_back(std::move(packed.value()));
    }
    return set_query(std::move(field), std::move(points), 1);
}

Result<std::shared_ptr<Query>> FloatPoint::new_range_query(std::string field, float lower_value, float upper_value) {
    const std::array<float, 1> lower{lower_value};
    const std::array<float, 1> upper{upper_value};
    return new_range_query_n(std::move(field), lower, upper);
}

Result<std::shared_ptr<Query>> FloatPoint::new_range_query_n(std::string field, std::span<const float> lower_value,
                                                             std::span<const float> upper_value) {
    if (lower_value.size() != upper_value.size()) {
        return err(LuceneError::illegal_argument("dimension size must be equals to the number of lower dimensions"));
    }
    auto lower = pack(lower_value);
    if (!lower.has_value()) {
        return std::unexpected(std::move(lower.error()));
    }
    auto upper = pack(upper_value);
    if (!upper.has_value()) {
        return std::unexpected(std::move(upper.error()));
    }
    return range_query(std::move(field), std::move(lower.value()), std::move(upper.value()), lower_value.size());
}

// ---- DoublePoint ----

Result<std::vector<uint8_t>> DoublePoint::pack(std::span<const double> point) {
    return pack_values<double>(point, BYTES, [](double value, std::span<uint8_t> bytes, size_t offset) {
        NumericUtils::long_to_sortable_bytes(NumericUtils::double_to_sortable_long(value), bytes, offset);
    });
}

Result<Field> DoublePoint::create(std::string name, std::span<const double> point) {
    auto packed = pack(point);
    if (!packed.has_value()) {
        return std::unexpected(std::move(packed.error()));
    }
    return point_field(std::move(name), std::move(packed.value()), point.size(), BYTES);
}

Result<std::shared_ptr<Query>> DoublePoint::new_exact_query(std::string field, double value) {
    const std::array<double, 1> point{value};
    return exact_query<double>(std::move(field), point, BYTES,
                               [](double v, std::span<uint8_t> bytes, size_t offset) {
                                   NumericUtils::long_to_sortable_bytes(NumericUtils::double_to_sortable_long(v), bytes,
                                                                       offset);
                               });
}

Result<std::shared_ptr<Query>> DoublePoint::new_set_query(std::string field, std::span<const double> values) {
    if (values.empty()) {
        return err(LuceneError::illegal_argument("values must contain at least 1 value"));
    }
    std::vector<std::vector<uint8_t>> points;
    points.reserve(values.size());
    for (const double value : values) {
        const std::array<double, 1> point{value};
        auto packed = pack(point);
        if (!packed.has_value()) {
            return std::unexpected(std::move(packed.error()));
        }
        points.push_back(std::move(packed.value()));
    }
    return set_query(std::move(field), std::move(points), 1);
}

Result<std::shared_ptr<Query>> DoublePoint::new_range_query(std::string field, double lower_value,
                                                            double upper_value) {
    const std::array<double, 1> lower{lower_value};
    const std::array<double, 1> upper{upper_value};
    return new_range_query_n(std::move(field), lower, upper);
}

Result<std::shared_ptr<Query>> DoublePoint::new_range_query_n(std::string field, std::span<const double> lower_value,
                                                              std::span<const double> upper_value) {
    if (lower_value.size() != upper_value.size()) {
        return err(LuceneError::illegal_argument("dimension size must be equals to the number of lower dimensions"));
    }
    auto lower = pack(lower_value);
    if (!lower.has_value()) {
        return std::unexpected(std::move(lower.error()));
    }
    auto upper = pack(upper_value);
    if (!upper.has_value()) {
        return std::unexpected(std::move(upper.error()));
    }
    return range_query(std::move(field), std::move(lower.value()), std::move(upper.value()), lower_value.size());
}

// ---- BinaryPoint ----

namespace {

/// Checks that every dimension of `point` has the same length, 1 to 8 bytes.
[[nodiscard]] VoidResult check_binary_point(std::span<const std::vector<uint8_t>> point) {
    if (point.empty()) {
        return err(LuceneError::illegal_argument("point must contain at least 1 value"));
    }
    if (point.size() > kMaxPointDimensions) {
        return err(LuceneError::illegal_argument("point must contain at most 8 values"));
    }
    const size_t width = point[0].size();
    if (width == 0 || width > 8) {
        return err(LuceneError::illegal_argument("dimensions must be between 1 and 8 bytes"));
    }
    for (const auto &dimension : point) {
        if (dimension.size() != width) {
            return err(LuceneError::illegal_argument("all dimensions must be the same number of bytes"));
        }
    }
    return {};
}

} // namespace

Result<Field> BinaryPoint::create(std::string name, std::span<const std::vector<uint8_t>> point) {
    if (auto checked = check_binary_point(point); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    std::vector<uint8_t> packed;
    packed.reserve(point.size() * point[0].size());
    for (const auto &dimension : point) {
        packed.insert(packed.end(), dimension.begin(), dimension.end());
    }
    return point_field(std::move(name), std::move(packed), point.size(), point[0].size());
}

Result<std::shared_ptr<Query>> BinaryPoint::new_exact_query(std::string field, std::span<const uint8_t> value) {
    const std::vector<std::vector<uint8_t>> point{std::vector<uint8_t>(value.begin(), value.end())};
    return new_range_query_n(std::move(field), point, point);
}

Result<std::shared_ptr<Query>> BinaryPoint::new_set_query(std::string field,
                                                          std::span<const std::vector<uint8_t>> values) {
    if (values.empty()) {
        return err(LuceneError::illegal_argument("values must contain at least 1 value"));
    }
    std::vector<std::vector<uint8_t>> points(values.begin(), values.end());
    const size_t num_dims = values[0].size() == 0 ? 0 : 1;
    if (num_dims == 0) {
        return err(LuceneError::illegal_argument("dimensions must be between 1 and 8 bytes"));
    }
    return set_query(std::move(field), std::move(points), 1);
}

Result<std::shared_ptr<Query>> BinaryPoint::new_range_query(std::string field, std::span<const uint8_t> lower_value,
                                                            std::span<const uint8_t> upper_value) {
    if (lower_value.size() != upper_value.size()) {
        return err(LuceneError::illegal_argument("dimension size must be equals to the number of lower dimensions"));
    }
    const std::vector<std::vector<uint8_t>> lower{std::vector<uint8_t>(lower_value.begin(), lower_value.end())};
    const std::vector<std::vector<uint8_t>> upper{std::vector<uint8_t>(upper_value.begin(), upper_value.end())};
    return new_range_query_n(std::move(field), lower, upper);
}

Result<std::shared_ptr<Query>> BinaryPoint::new_range_query_n(std::string field,
                                                              std::span<const std::vector<uint8_t>> lower_value,
                                                              std::span<const std::vector<uint8_t>> upper_value) {
    if (auto checked = check_binary_point(lower_value); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = check_binary_point(upper_value); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (lower_value.size() != upper_value.size() || lower_value[0].size() != upper_value[0].size()) {
        return err(LuceneError::illegal_argument("dimension size must be equals to the number of lower dimensions"));
    }
    std::vector<uint8_t> lower;
    std::vector<uint8_t> upper;
    for (size_t dim = 0; dim < lower_value.size(); ++dim) {
        lower.insert(lower.end(), lower_value[dim].begin(), lower_value[dim].end());
        upper.insert(upper.end(), upper_value[dim].begin(), upper_value[dim].end());
    }
    return range_query(std::move(field), std::move(lower), std::move(upper), lower_value.size());
}

} // namespace lucene
