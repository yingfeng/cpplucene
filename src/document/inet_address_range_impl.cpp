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

module lucene_core:inet_address_range.impl;

import :inet_address_range;

import std;

import :error;
import :field_type;
import :index_types;
import :indexable_field;
import :inet_address_point;
import :range_field_query;
import :types;

namespace lucene {

namespace {

/// Renders one dimension of the encoded ranges for [`RangeFieldQuery::to_string`].
class InetAddressRangeFormat final : public RangeFieldFormat {
public:
    [[nodiscard]] Result<std::string> format(std::span<const uint8_t> ranges, size_t dimension) const override {
        (void)dimension;
        return InetAddressRange::format_range(ranges);
    }
};

[[nodiscard]] std::shared_ptr<const RangeFieldFormat> inet_address_range_format() {
    static const std::shared_ptr<const RangeFieldFormat> format = std::make_shared<InetAddressRangeFormat>();
    return format;
}

} // namespace

Result<InetAddressRange> InetAddressRange::create(std::string name, const Address &min, const Address &max) {
    auto type_for_field = type();
    if (!type_for_field.has_value()) {
        return std::unexpected(std::move(type_for_field.error()));
    }
    auto bytes = encode(min, max);
    if (!bytes.has_value()) {
        return std::unexpected(std::move(bytes.error()));
    }
    auto field = Field::from_bytes(std::move(name), std::move(bytes.value()), std::move(type_for_field.value()));
    if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
    }
    return InetAddressRange(std::move(field.value()));
}

Result<FieldType> InetAddressRange::type() {
    FieldType type;
    if (auto set = type.set_dimensions(2, BYTES); !set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }
    type.freeze();
    return type;
}

Result<std::vector<uint8_t>> InetAddressRange::encode(const Address &min, const Address &max) {
    if (std::lexicographical_compare(max.begin(), max.end(), min.begin(), min.end())) {
        return err(LuceneError::illegal_argument("min value cannot be greater than max value for InetAddressRange field"));
    }
    std::vector<uint8_t> bytes(2 * BYTES, 0);
    std::copy(min.begin(), min.end(), bytes.begin());
    std::copy(max.begin(), max.end(), bytes.begin() + BYTES);
    return bytes;
}

Result<InetAddressRange::Address> InetAddressRange::decode_min(std::span<const uint8_t> bytes) {
    if (bytes.size() < 2 * BYTES) {
        return err(LuceneError::illegal_argument("the encoded range is too short"));
    }
    Address address{};
    std::copy(bytes.begin(), bytes.begin() + BYTES, address.begin());
    return address;
}

Result<InetAddressRange::Address> InetAddressRange::decode_max(std::span<const uint8_t> bytes) {
    if (bytes.size() < 2 * BYTES) {
        return err(LuceneError::illegal_argument("the encoded range is too short"));
    }
    Address address{};
    std::copy(bytes.begin() + BYTES, bytes.begin() + 2 * BYTES, address.begin());
    return address;
}

Result<std::string> InetAddressRange::format_range(std::span<const uint8_t> bytes) {
    auto min = decode_min(bytes);
    if (!min.has_value()) {
        return std::unexpected(std::move(min.error()));
    }
    auto max = decode_max(bytes);
    if (!max.has_value()) {
        return std::unexpected(std::move(max.error()));
    }
    return std::format("[{} : {}]", InetAddressPoint::to_string(min.value()),
                       InetAddressPoint::to_string(max.value()));
}

Result<InetAddressRange::Address> InetAddressRange::get_min() const {
    auto bytes = field_.binary_value();
    if (!bytes.has_value()) {
        return err(LuceneError::illegal_state("the range field holds no bytes"));
    }
    return decode_min(bytes.value());
}

Result<InetAddressRange::Address> InetAddressRange::get_max() const {
    auto bytes = field_.binary_value();
    if (!bytes.has_value()) {
        return err(LuceneError::illegal_state("the range field holds no bytes"));
    }
    return decode_max(bytes.value());
}

VoidResult InetAddressRange::set_range_values(const Address &min, const Address &max) {
    auto bytes = encode(min, max);
    if (!bytes.has_value()) {
        return std::unexpected(std::move(bytes.error()));
    }
    return field_.set_bytes_value(std::move(bytes.value()));
}

Result<RangeFieldQuery> InetAddressRange::new_relation_query(std::string field, const Address &min,
                                                             const Address &max, QueryType relation) {
    auto ranges = encode(min, max);
    if (!ranges.has_value()) {
        return std::unexpected(std::move(ranges.error()));
    }
    return RangeFieldQuery::create(std::move(field), std::move(ranges.value()), 1, relation,
                                   inet_address_range_format());
}

Result<RangeFieldQuery> InetAddressRange::new_intersects_query(std::string field, const Address &min,
                                                               const Address &max) {
    return new_relation_query(std::move(field), min, max, QueryType::Intersects);
}

Result<RangeFieldQuery> InetAddressRange::new_contains_query(std::string field, const Address &min,
                                                             const Address &max) {
    return new_relation_query(std::move(field), min, max, QueryType::Contains);
}

Result<RangeFieldQuery> InetAddressRange::new_within_query(std::string field, const Address &min, const Address &max) {
    return new_relation_query(std::move(field), min, max, QueryType::Within);
}

Result<RangeFieldQuery> InetAddressRange::new_crosses_query(std::string field, const Address &min,
                                                            const Address &max) {
    return new_relation_query(std::move(field), min, max, QueryType::Crosses);
}

std::string InetAddressRange::to_string() const {
    std::string result = "InetAddressRange <" + std::string(field_.name()) + ": ";
    auto bytes = field_.binary_value();
    if (!bytes.has_value()) {
        return result + "Unsupported range data>";
    }
    auto range = format_range(bytes.value());
    if (!range.has_value()) {
        return result + "Unsupported range data>";
    }
    return result + range.value() + ">";
}

} // namespace lucene
