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

export module lucene_core:geo_encoding_utils;

import :types;
import std;

import :error;

namespace lucene {

/// Quantizes latitude/longitude doubles into the 32-bit sortable integers the
/// point index stores.
///
/// Port of `GeoEncodingUtils` (`core/geo/geo_encoding_utils.rs`).
///
/// **Deviation:** the reference implementation keeps the two search-time
/// predicate builders (`create_distance_predicate`, `create_component_predicate`)
/// in this file, which makes it depend on the geo shapes; the port puts them in
/// `:geo_predicates` so this module stays the pure codec that the shapes can
/// depend on.
export struct GeoEncodingUtils {
    /// Number of bits used for quantizing latitude and longitude values.
    static constexpr uint32_t BITS = 32;

    /// The encoded value of [`GeoUtils::MIN_LON_INCL`].
    [[nodiscard]] static int32_t min_lon_encoded();
    /// The encoded value of [`GeoUtils::MAX_LON_INCL`].
    [[nodiscard]] static int32_t max_lon_encoded();

    /// Quantizes `latitude` into 32 bits, rounding down (toward -90).
    [[nodiscard]] static Result<int32_t> encode_latitude(double latitude);
    /// Quantizes `latitude` into 32 bits, rounding up (toward +90).
    [[nodiscard]] static Result<int32_t> encode_latitude_ceil(double latitude);

    /// Quantizes `longitude` into 32 bits, rounding down (toward -180).
    [[nodiscard]] static Result<int32_t> encode_longitude(double longitude);
    /// Quantizes `longitude` into 32 bits, rounding up (toward +180).
    [[nodiscard]] static Result<int32_t> encode_longitude_ceil(double longitude);

    /// The latitude a quantized value stands for.
    [[nodiscard]] static double decode_latitude(int32_t encoded);
    /// The latitude stored at `offset` in `src`.
    [[nodiscard]] static double decode_latitude_from_bytes(std::span<const uint8_t> src, size_t offset);

    /// The longitude a quantized value stands for.
    [[nodiscard]] static double decode_longitude(int32_t encoded);
    /// The longitude stored at `offset` in `src`.
    [[nodiscard]] static double decode_longitude_from_bytes(std::span<const uint8_t> src, size_t offset);
};

} // namespace lucene
