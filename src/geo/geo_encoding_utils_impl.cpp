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

module lucene_core:geo_encoding_utils.impl;

import :geo_encoding_utils;

import std;

import :error;
import :geo_utils;
import :numeric_utils;
import :types;

namespace lucene {

namespace {

constexpr double LAT_SCALE = static_cast<double>(uint64_t{1} << GeoEncodingUtils::BITS) / 180.0;
constexpr double LAT_DECODE = 1.0 / LAT_SCALE;
constexpr double LON_SCALE = static_cast<double>(uint64_t{1} << GeoEncodingUtils::BITS) / 360.0;
constexpr double LON_DECODE = 1.0 / LON_SCALE;

/// The largest double below `value` (`f64::next_down`).
[[nodiscard]] double next_down(double value) {
    return std::nextafter(value, -std::numeric_limits<double>::infinity());
}

} // namespace

int32_t GeoEncodingUtils::min_lon_encoded() {
    static const int32_t encoded = encode_longitude(GeoUtils::MIN_LON_INCL).value();
    return encoded;
}

int32_t GeoEncodingUtils::max_lon_encoded() {
    static const int32_t encoded = encode_longitude(GeoUtils::MAX_LON_INCL).value();
    return encoded;
}

Result<int32_t> GeoEncodingUtils::encode_latitude(double latitude) {
    if (auto checked = GeoUtils::check_latitude(latitude); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    // The maximum possible value cannot be encoded without overflow.
    if (latitude == 90.0) {
        latitude = next_down(latitude);
    }
    return static_cast<int32_t>(std::floor(latitude / LAT_DECODE));
}

Result<int32_t> GeoEncodingUtils::encode_latitude_ceil(double latitude) {
    if (auto checked = GeoUtils::check_latitude(latitude); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (latitude == 90.0) {
        latitude = next_down(latitude);
    }
    return static_cast<int32_t>(std::ceil(latitude / LAT_DECODE));
}

Result<int32_t> GeoEncodingUtils::encode_longitude(double longitude) {
    if (auto checked = GeoUtils::check_longitude(longitude); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (longitude == 180.0) {
        longitude = next_down(longitude);
    }
    return static_cast<int32_t>(std::floor(longitude / LON_DECODE));
}

Result<int32_t> GeoEncodingUtils::encode_longitude_ceil(double longitude) {
    if (auto checked = GeoUtils::check_longitude(longitude); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (longitude == 180.0) {
        longitude = next_down(longitude);
    }
    return static_cast<int32_t>(std::ceil(longitude / LON_DECODE));
}

double GeoEncodingUtils::decode_latitude(int32_t encoded) {
    return static_cast<double>(encoded) * LAT_DECODE;
}

double GeoEncodingUtils::decode_latitude_from_bytes(std::span<const uint8_t> src, size_t offset) {
    return decode_latitude(NumericUtils::sortable_bytes_to_int(src, offset));
}

double GeoEncodingUtils::decode_longitude(int32_t encoded) {
    return static_cast<double>(encoded) * LON_DECODE;
}

double GeoEncodingUtils::decode_longitude_from_bytes(std::span<const uint8_t> src, size_t offset) {
    return decode_longitude(NumericUtils::sortable_bytes_to_int(src, offset));
}

} // namespace lucene
