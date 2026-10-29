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

module lucene_core:half_float_point.impl;

import :half_float_point;

import :error;
import :indexable_field;
import :field_type;
import :point_in_set_query;
import :point_range_query;
import :query;
import :types;

import std;

namespace lucene {

namespace {

constexpr size_t kMaxPointDimensions = 8;

/// Divide by `2^shift` and round to the nearest integer, ties to even.
[[nodiscard]] int32_t round_shift(int32_t value, int32_t shift) {
    value += static_cast<int32_t>(1U << (shift - 1));
    value -= static_cast<int32_t>((static_cast<uint32_t>(value) >> shift) & 1U);
    return static_cast<int32_t>(static_cast<uint32_t>(value) >> shift);
}

template <typename Encode>
[[nodiscard]] Result<std::vector<uint8_t>> pack_values(std::span<const float> values, Encode encode) {
    if (values.empty()) {
        return err(LuceneError::illegal_argument("point must contain at least 1 value"));
    }
    if (values.size() > kMaxPointDimensions) {
        return err(LuceneError::illegal_argument("point must contain at most 8 values"));
    }
    std::vector<uint8_t> packed(values.size() * HalfFloatPoint::BYTES, 0);
    for (size_t dim = 0; dim < values.size(); ++dim) {
        encode(values[dim], packed, dim * HalfFloatPoint::BYTES);
    }
    return packed;
}

[[nodiscard]] Result<Field> point_field(std::string name, std::vector<uint8_t> packed, size_t dimensions) {
    FieldType type;
    if (auto set = type.set_dimensions(dimensions, HalfFloatPoint::BYTES); !set.has_value()) {
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

} // namespace

float HalfFloatPoint::next_up(float v) {
    if (std::isnan(v) || v == std::numeric_limits<float>::infinity()) {
        return v;
    }
    const int16_t sortable = half_float_to_sortable_short(v);
    float result = sortable_short_to_half_float(sortable);
    if (result <= v) {
        result = sortable_short_to_half_float(static_cast<int16_t>(static_cast<uint16_t>(sortable) + 1U));
    }
    return result;
}

float HalfFloatPoint::next_down(float v) {
    if (std::isnan(v) || v == -std::numeric_limits<float>::infinity()) {
        return v;
    }
    const int16_t sortable = half_float_to_sortable_short(v);
    float result = sortable_short_to_half_float(sortable);
    if (result >= v) {
        result = sortable_short_to_half_float(static_cast<int16_t>(static_cast<uint16_t>(sortable) - 1U));
    }
    return result;
}

int16_t HalfFloatPoint::half_float_to_sortable_short(float v) {
    return sortable_short_bits(half_float_to_short_bits(v));
}

float HalfFloatPoint::sortable_short_to_half_float(int16_t bits) {
    return short_bits_to_half_float(sortable_short_bits(bits));
}

int16_t HalfFloatPoint::sortable_short_bits(int16_t bits) {
    const uint16_t raw = static_cast<uint16_t>(bits);
    const uint16_t flipped = static_cast<uint16_t>(raw ^ (static_cast<uint16_t>(bits >> 15) & 0x7FFFU));
    return static_cast<int16_t>(flipped);
}

int16_t HalfFloatPoint::half_float_to_short_bits(float v) {
    const uint32_t float_bits = std::isnan(v) ? 0x7FC00000U : std::bit_cast<uint32_t>(v);
    const uint32_t sign = float_bits >> 31;
    int32_t exponent = static_cast<int32_t>((float_bits >> 23) & 0xFFU);
    int32_t mantissa = static_cast<int32_t>(float_bits & 0x7FFFFFU);

    if (exponent == 0xFF) {
        exponent = 0x1F;
        mantissa >>= 23 - 10;
    } else if (exponent == 0x00) {
        mantissa = 0;
    } else {
        exponent = exponent - 127 + 15;
        if (exponent >= 0x1F) {
            exponent = 0x1F;
            mantissa = 0;
        } else if (exponent <= 0) {
            const int32_t shift = 23 - 10 - exponent + 1;
            if (shift >= 32) {
                exponent = 0;
                mantissa = 0;
            } else {
                mantissa |= 0x800000;
                mantissa = round_shift(mantissa, shift);
                exponent = mantissa >> 10;
                mantissa &= 0x3FF;
            }
        } else {
            mantissa = round_shift((exponent << 23) | mantissa, 23 - 10);
            exponent = mantissa >> 10;
            mantissa &= 0x3FF;
        }
    }

    const uint32_t bits =
        ((sign << 15) | (static_cast<uint32_t>(exponent) << 10) | static_cast<uint32_t>(mantissa)) & 0xFFFFU;
    return static_cast<int16_t>(static_cast<uint16_t>(bits));
}

float HalfFloatPoint::short_bits_to_half_float(int16_t bits) {
    const uint32_t raw = static_cast<uint16_t>(bits);
    const uint32_t sign = raw >> 15;
    int32_t exponent = static_cast<int32_t>((raw >> 10) & 0x1FU);
    int32_t mantissa = static_cast<int32_t>(raw & 0x3FFU);
    if (exponent == 0x1F) {
        exponent = 0xFF;
        mantissa <<= 23 - 10;
    } else if (!(mantissa == 0 && exponent == 0)) {
        if (exponent == 0) {
            const int32_t shift = static_cast<int32_t>(std::countl_zero(static_cast<uint32_t>(mantissa))) - (32 - 11);
            mantissa = (mantissa << shift) & 0x3FF;
            exponent = exponent - shift + 1;
        }
        exponent = exponent + 127 - 15;
        mantissa <<= 23 - 10;
    }

    const uint32_t result = (sign << 31) | (static_cast<uint32_t>(exponent) << 23) | static_cast<uint32_t>(mantissa);
    return std::bit_cast<float>(result);
}

void HalfFloatPoint::short_to_sortable_bytes(int16_t value, std::span<uint8_t> result, size_t offset) {
    const uint16_t encoded = static_cast<uint16_t>(static_cast<uint16_t>(value) ^ 0x8000U);
    result[offset] = static_cast<uint8_t>(encoded >> 8);
    result[offset + 1] = static_cast<uint8_t>(encoded & 0xFFU);
}

int16_t HalfFloatPoint::sortable_bytes_to_short(std::span<const uint8_t> encoded, size_t offset) {
    const uint16_t raw = static_cast<uint16_t>((static_cast<uint16_t>(encoded[offset]) << 8) |
                                               static_cast<uint16_t>(encoded[offset + 1]));
    return static_cast<int16_t>(static_cast<uint16_t>(raw ^ 0x8000U));
}

void HalfFloatPoint::encode_dimension(float value, std::span<uint8_t> dest, size_t offset) {
    short_to_sortable_bytes(half_float_to_sortable_short(value), dest, offset);
}

float HalfFloatPoint::decode_dimension(std::span<const uint8_t> value, size_t offset) {
    return sortable_short_to_half_float(sortable_bytes_to_short(value, offset));
}

Result<std::vector<uint8_t>> HalfFloatPoint::pack(std::span<const float> point) {
    return pack_values(point, [](float value, std::span<uint8_t> bytes, size_t offset) {
        encode_dimension(value, bytes, offset);
    });
}

Result<Field> HalfFloatPoint::create(std::string name, std::span<const float> point) {
    auto packed = pack(point);
    if (!packed.has_value()) {
        return std::unexpected(std::move(packed.error()));
    }
    return point_field(std::move(name), std::move(packed.value()), point.size());
}

Result<std::shared_ptr<Query>> HalfFloatPoint::new_exact_query(std::string field, float value) {
    return new_range_query(std::move(field), value, value);
}

Result<std::shared_ptr<Query>> HalfFloatPoint::new_range_query(std::string field, float lower_value,
                                                               float upper_value) {
    const std::array<float, 1> lower{lower_value};
    const std::array<float, 1> upper{upper_value};
    return new_range_query_n(std::move(field), lower, upper);
}

Result<std::shared_ptr<Query>> HalfFloatPoint::new_range_query_n(std::string field,
                                                                 std::span<const float> lower_value,
                                                                 std::span<const float> upper_value) {
    if (lower_value.size() != upper_value.size()) {
        return err(LuceneError::illegal_argument("lowerValue.length != upperValue.length"));
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

Result<std::shared_ptr<Query>> HalfFloatPoint::new_set_query(std::string field, std::span<const float> values) {
    if (values.empty()) {
        return err(LuceneError::illegal_argument("values must not be empty"));
    }
    std::vector<float> sorted(values.begin(), values.end());
    std::ranges::sort(sorted, [](float left, float right) {
        if (std::isnan(left) || std::isnan(right)) {
            return !std::isnan(left) && std::isnan(right);
        }
        return left < right;
    });
    std::vector<std::vector<uint8_t>> points;
    points.reserve(sorted.size());
    for (const float value : sorted) {
        auto packed = pack(std::span<const float>(&value, 1));
        if (!packed.has_value()) {
            return std::unexpected(std::move(packed.error()));
        }
        points.push_back(std::move(packed.value()));
    }
    auto query = PointInSetQuery::create(std::move(field), std::move(points), 1);
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<PointInSetQuery>(std::move(query.value())));
}

} // namespace lucene
