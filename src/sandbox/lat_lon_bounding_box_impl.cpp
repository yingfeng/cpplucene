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

module lucene_core:lat_lon_bounding_box.impl;

import :lat_lon_bounding_box;

import :error;
import :field_type;
import :geo_encoding_utils;
import :indexable_field;
import :numeric_utils;
import :range_field_query;
import :types;

import std;

namespace lucene {

namespace {

constexpr size_t kGeoDimensions = 2;
constexpr size_t kIntBytes = sizeof(int32_t);

/// Renders one dimension of the encoded box for [`RangeFieldQuery::to_string`].
class LatLonBoundingBoxFormat final : public RangeFieldFormat {
public:
    [[nodiscard]] Result<std::string> format(std::span<const uint8_t> ranges, size_t dimension) const override {
        if (dimension >= 2) {
            return err(LuceneError::illegal_argument(std::format("invalid dimension [{}] in toString", dimension)));
        }
        const size_t offset = dimension * 2 * kIntBytes;
        const double lat = GeoEncodingUtils::decode_latitude_from_bytes(ranges, offset);
        const double lon = GeoEncodingUtils::decode_longitude_from_bytes(ranges, offset + kIntBytes);
        return std::format("{}={},{}", dimension == 0 ? "min" : "max", lat, lon);
    }
};

[[nodiscard]] std::shared_ptr<const RangeFieldFormat> lat_lon_format() {
    static const std::shared_ptr<const RangeFieldFormat> format = std::make_shared<LatLonBoundingBoxFormat>();
    return format;
}

[[nodiscard]] VoidResult encode_point(double lat, double lon, std::span<uint8_t> result, size_t offset) {
    auto encoded_lat = GeoEncodingUtils::encode_latitude(lat);
    if (!encoded_lat.has_value()) {
        return std::unexpected(std::move(encoded_lat.error()));
    }
    auto encoded_lon = GeoEncodingUtils::encode_longitude(lon);
    if (!encoded_lon.has_value()) {
        return std::unexpected(std::move(encoded_lon.error()));
    }
    NumericUtils::int_to_sortable_bytes(encoded_lat.value(), result, offset);
    NumericUtils::int_to_sortable_bytes(encoded_lon.value(), result, offset + kIntBytes);
    return {};
}

} // namespace

VoidResult LatLonBoundingBox::check_args(double min_lat, double min_lon, double max_lat, double max_lon) {
    if (min_lon > max_lon) {
        return err(LuceneError::illegal_argument(
            std::format("cannot have minLon [{}] exceed maxLon [{}].", min_lon, max_lon)));
    }
    if (min_lat > max_lat) {
        return err(LuceneError::illegal_argument(
            std::format("cannot have minLat [{}] exceed maxLat [{}].", min_lat, max_lat)));
    }
    return {};
}

Result<std::vector<uint8_t>> LatLonBoundingBox::encode(double min_lat, double min_lon, double max_lat,
                                                       double max_lon) {
    if (auto checked = check_args(min_lat, min_lon, max_lat, max_lon); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    std::vector<uint8_t> bytes(4 * BYTES, 0);
    if (auto first = encode_point(min_lat, min_lon, bytes, 0); !first.has_value()) {
        return std::unexpected(std::move(first.error()));
    }
    if (auto second = encode_point(max_lat, max_lon, bytes, 2 * BYTES); !second.has_value()) {
        return std::unexpected(std::move(second.error()));
    }
    return bytes;
}

Result<Field> LatLonBoundingBox::create(std::string name, double min_lat, double min_lon, double max_lat,
                                        double max_lon) {
    auto bytes = encode(min_lat, min_lon, max_lat, max_lon);
    if (!bytes.has_value()) {
        return std::unexpected(std::move(bytes.error()));
    }
    FieldType type;
    if (auto dimensions = type.set_dimensions(kGeoDimensions * 2, BYTES); !dimensions.has_value()) {
        return std::unexpected(std::move(dimensions.error()));
    }
    type.freeze();
    return Field::from_bytes(std::move(name), std::move(bytes.value()), std::move(type));
}

Result<std::shared_ptr<Query>> LatLonBoundingBox::new_relation_query(std::string field, double min_lat,
                                                                    double min_lon, double max_lat, double max_lon,
                                                                    QueryType query_type) {
    auto bytes = encode(min_lat, min_lon, max_lat, max_lon);
    if (!bytes.has_value()) {
        return std::unexpected(std::move(bytes.error()));
    }
    auto query = RangeFieldQuery::create(std::move(field), std::move(bytes.value()), kGeoDimensions, query_type,
                                         lat_lon_format());
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<RangeFieldQuery>(std::move(query.value())));
}

Result<std::shared_ptr<Query>> LatLonBoundingBox::new_intersects_query(std::string field, double min_lat,
                                                                      double min_lon, double max_lat,
                                                                      double max_lon) {
    return new_relation_query(std::move(field), min_lat, min_lon, max_lat, max_lon, QueryType::Intersects);
}

Result<std::shared_ptr<Query>> LatLonBoundingBox::new_within_query(std::string field, double min_lat,
                                                                  double min_lon, double max_lat, double max_lon) {
    return new_relation_query(std::move(field), min_lat, min_lon, max_lat, max_lon, QueryType::Within);
}

Result<std::shared_ptr<Query>> LatLonBoundingBox::new_contains_query(std::string field, double min_lat,
                                                                    double min_lon, double max_lat, double max_lon) {
    return new_relation_query(std::move(field), min_lat, min_lon, max_lat, max_lon, QueryType::Contains);
}

Result<std::shared_ptr<Query>> LatLonBoundingBox::new_crosses_query(std::string field, double min_lat,
                                                                   double min_lon, double max_lat, double max_lon) {
    return new_relation_query(std::move(field), min_lat, min_lon, max_lat, max_lon, QueryType::Crosses);
}

std::string LatLonBoundingBox::to_string(std::span<const uint8_t> encoded) {
    if (encoded.size() < 4 * BYTES) {
        return "LatLonBoundingBox <invalid>";
    }
    const double min_lat = GeoEncodingUtils::decode_latitude_from_bytes(encoded, 0);
    const double min_lon = GeoEncodingUtils::decode_longitude_from_bytes(encoded, kIntBytes);
    const double max_lat = GeoEncodingUtils::decode_latitude_from_bytes(encoded, 2 * BYTES);
    const double max_lon = GeoEncodingUtils::decode_longitude_from_bytes(encoded, 3 * BYTES);
    return std::format("LatLonBoundingBox <[{},{}],[{},{}]>", min_lat, min_lon, max_lat, max_lon);
}

} // namespace lucene
