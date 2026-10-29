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

module lucene_core:big_integer_point.impl;

import :big_integer_point;

import :error;
import :field_type;
import :indexable_field;
import :numeric_utils;
import :point_in_set_query;
import :point_range_query;
import :query;
import :types;

import std;

namespace lucene {

namespace {

constexpr size_t kMaxPointDimensions = 8;
constexpr size_t kInt128Bytes = 16;
constexpr int kInt128Bits = 128;

using UInt128 = unsigned __int128;

[[nodiscard]] Result<std::vector<uint8_t>> pack_values(std::span<const Int128> values) {
    if (values.empty()) {
        return err(LuceneError::illegal_argument("point must contain at least 1 value"));
    }
    if (values.size() > kMaxPointDimensions) {
        return err(LuceneError::illegal_argument("point must contain at most 8 values"));
    }
    std::vector<uint8_t> packed(values.size() * kInt128Bytes, 0);
    for (size_t dim = 0; dim < values.size(); ++dim) {
        NumericUtils::int128_to_sortable_bytes(values[dim], packed, dim * kInt128Bytes);
    }
    return packed;
}

[[nodiscard]] Result<Field> point_field(std::string name, std::vector<uint8_t> packed, size_t dimensions) {
    FieldType type;
    if (auto set = type.set_dimensions(dimensions, kInt128Bytes); !set.has_value()) {
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

Int128 BigIntegerPoint::min_value() noexcept {
    return static_cast<Int128>(static_cast<UInt128>(1) << (kInt128Bits - 1));
}

Int128 BigIntegerPoint::max_value() noexcept {
    return static_cast<Int128>((static_cast<UInt128>(1) << (kInt128Bits - 1)) - 1);
}

Result<std::vector<uint8_t>> BigIntegerPoint::pack(std::span<const Int128> point) {
    return pack_values(point);
}

Result<Field> BigIntegerPoint::create(std::string name, std::span<const Int128> point) {
    auto packed = pack(point);
    if (!packed.has_value()) {
        return std::unexpected(std::move(packed.error()));
    }
    return point_field(std::move(name), std::move(packed.value()), point.size());
}

void BigIntegerPoint::encode_dimension(Int128 value, std::span<uint8_t> dest, size_t offset) {
    NumericUtils::int128_to_sortable_bytes(value, dest, offset);
}

Int128 BigIntegerPoint::decode_dimension(std::span<const uint8_t> value, size_t offset) {
    return NumericUtils::sortable_bytes_to_int128(value, offset);
}

Result<Int128> BigIntegerPoint::parse(std::string_view decimal) {
    if (decimal.empty()) {
        return err(LuceneError::number_format("empty number"));
    }
    size_t pos = 0;
    bool negative = false;
    if (decimal[pos] == '+' || decimal[pos] == '-') {
        negative = decimal[pos] == '-';
        ++pos;
    }
    if (pos == decimal.size()) {
        return err(LuceneError::number_format("no digits in number"));
    }
    // Accumulate the magnitude in 128 bits, checking the sign-specific limit.
    const UInt128 limit = negative ? (static_cast<UInt128>(1) << (kInt128Bits - 1))
                                   : (static_cast<UInt128>(1) << (kInt128Bits - 1)) - 1;
    UInt128 magnitude = 0;
    for (; pos < decimal.size(); ++pos) {
        const char c = decimal[pos];
        if (c < '0' || c > '9') {
            return err(LuceneError::number_format("not a decimal number"));
        }
        const auto digit = static_cast<UInt128>(c - '0');
        if (magnitude > (limit - digit) / 10) {
            return err(LuceneError::number_overflow("value does not fit in 128 bits"));
        }
        magnitude = magnitude * 10 + digit;
    }
    if (!negative) {
        return static_cast<Int128>(magnitude);
    }
    if (magnitude == (static_cast<UInt128>(1) << (kInt128Bits - 1))) {
        return min_value();
    }
    return static_cast<Int128>(-static_cast<Int128>(magnitude));
}

std::string BigIntegerPoint::to_string(Int128 value) {
    if (value == 0) {
        return "0";
    }
    const bool negative = value < 0;
    UInt128 magnitude = negative ? static_cast<UInt128>(0) - static_cast<UInt128>(value) : static_cast<UInt128>(value);
    std::string digits;
    while (magnitude != 0) {
        digits.push_back(static_cast<char>('0' + static_cast<int>(magnitude % 10)));
        magnitude /= 10;
    }
    if (negative) {
        digits.push_back('-');
    }
    std::ranges::reverse(digits);
    return digits;
}

Result<std::shared_ptr<Query>> BigIntegerPoint::new_exact_query(std::string field, Int128 value) {
    return new_range_query(std::move(field), value, value);
}

Result<std::shared_ptr<Query>> BigIntegerPoint::new_range_query(std::string field, Int128 lower_value,
                                                                Int128 upper_value) {
    const std::array<Int128, 1> lower{lower_value};
    const std::array<Int128, 1> upper{upper_value};
    return new_range_query_n(std::move(field), lower, upper);
}

Result<std::shared_ptr<Query>> BigIntegerPoint::new_range_query_n(std::string field,
                                                                  std::span<const Int128> lower_value,
                                                                  std::span<const Int128> upper_value) {
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

Result<std::shared_ptr<Query>> BigIntegerPoint::new_set_query(std::string field, std::span<const Int128> values) {
    if (values.empty()) {
        return err(LuceneError::illegal_argument("values must not be empty"));
    }
    std::vector<Int128> sorted(values.begin(), values.end());
    std::ranges::sort(sorted);
    std::vector<std::vector<uint8_t>> points;
    points.reserve(sorted.size());
    for (const Int128 value : sorted) {
        const std::array<Int128, 1> single{value};
        auto packed = pack(single);
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
