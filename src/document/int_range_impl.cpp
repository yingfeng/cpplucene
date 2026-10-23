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

module lucene_core:int_range.impl;

import :int_range;

import std;

import :binary_range_field_range_query;
import :error;
import :field_type;
import :index_types;
import :indexable_field;
import :numeric_utils;
import :range_field_query;
import :types;

namespace lucene {

namespace {

/// Renders one dimension of the encoded ranges for [`RangeFieldQuery::to_string`].
class IntRangeFormat final : public RangeFieldFormat {
public:
    [[nodiscard]] Result<std::string> format(std::span<const uint8_t> ranges, size_t dimension) const override {
        const size_t bytes_per_dim = IntRange::BYTES;
        const size_t min_offset = dimension * bytes_per_dim;
        const size_t max_offset = min_offset + bytes_per_dim * (ranges.size() / (2 * bytes_per_dim));
        const int32_t min = NumericUtils::sortable_bytes_to_int(ranges, min_offset);
        const int32_t max = NumericUtils::sortable_bytes_to_int(ranges, max_offset);
        return std::format("[{} : {}]", min, max);
    }
};

[[nodiscard]] std::shared_ptr<const RangeFieldFormat> int_range_format() {
    static const std::shared_ptr<const RangeFieldFormat> format = std::make_shared<IntRangeFormat>();
    return format;
}

} // namespace

Result<IntRange> IntRange::create(std::string name, std::span<const int32_t> min, std::span<const int32_t> max) {
    if (auto checked = check_args(min, max); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    auto type = type_for(min.size());
    if (!type.has_value()) {
        return std::unexpected(std::move(type.error()));
    }
    auto bytes = encode(min, max);
    if (!bytes.has_value()) {
        return std::unexpected(std::move(bytes.error()));
    }
    auto field = Field::from_bytes(std::move(name), std::move(bytes.value()), std::move(type.value()));
    if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
    }
    return IntRange(std::move(field.value()));
}

Result<FieldType> IntRange::type_for(size_t dimensions) {
    if (dimensions > MAX_DIMENSIONS) {
        return err(LuceneError::illegal_argument("IntRange does not support greater than 4 dimensions"));
    }
    FieldType type;
    if (auto set = type.set_dimensions(dimensions * 2, BYTES); !set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }
    type.freeze();
    return type;
}

VoidResult IntRange::check_args(std::span<const int32_t> min, std::span<const int32_t> max) {
    if (min.empty() || max.empty()) {
        return err(LuceneError::illegal_argument("min/max range values cannot be null or empty"));
    }
    if (min.size() != max.size()) {
        return err(LuceneError::illegal_argument("min/max ranges must agree"));
    }
    if (min.size() > MAX_DIMENSIONS) {
        return err(LuceneError::illegal_argument("IntRange does not support greater than 4 dimensions"));
    }
    return {};
}

VoidResult IntRange::verify_and_encode(std::span<const int32_t> min, std::span<const int32_t> max,
                                       std::span<uint8_t> bytes) {
    const size_t dimensions = min.size();
    size_t min_offset = 0;
    size_t max_offset = dimensions * BYTES;
    for (size_t dim = 0; dim < dimensions; ++dim) {
        if (min[dim] > max[dim]) {
            return err(LuceneError::illegal_argument(std::format("min value ({}) is greater than max value ({})",
                                                                min[dim], max[dim])));
        }
        NumericUtils::int_to_sortable_bytes(min[dim], bytes, min_offset);
        NumericUtils::int_to_sortable_bytes(max[dim], bytes, max_offset);
        min_offset += BYTES;
        max_offset += BYTES;
    }
    return {};
}

Result<std::vector<uint8_t>> IntRange::encode(std::span<const int32_t> min, std::span<const int32_t> max) {
    if (auto checked = check_args(min, max); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    std::vector<uint8_t> bytes(2 * BYTES * min.size(), 0);
    if (auto encoded = verify_and_encode(min, max, bytes); !encoded.has_value()) {
        return std::unexpected(std::move(encoded.error()));
    }
    return bytes;
}

Result<int32_t> IntRange::decode_min(std::span<const uint8_t> bytes, size_t dimension) {
    const size_t offset = dimension * BYTES;
    if (offset + BYTES > bytes.size()) {
        return err(LuceneError::illegal_argument("dimension out of range"));
    }
    return NumericUtils::sortable_bytes_to_int(bytes, offset);
}

Result<int32_t> IntRange::decode_max(std::span<const uint8_t> bytes, size_t dimension) {
    const size_t offset = bytes.size() / 2 + dimension * BYTES;
    if (offset + BYTES > bytes.size()) {
        return err(LuceneError::illegal_argument("dimension out of range"));
    }
    return NumericUtils::sortable_bytes_to_int(bytes, offset);
}

Result<int32_t> IntRange::get_min(size_t dimension) const {
    auto bytes = field_.binary_value();
    if (!bytes.has_value()) {
        return err(LuceneError::illegal_state("the range field holds no bytes"));
    }
    if (dimension >= field_.field_type().point_dimension_count() / 2) {
        return err(LuceneError::illegal_argument("dimension out of range"));
    }
    return decode_min(bytes.value(), dimension);
}

Result<int32_t> IntRange::get_max(size_t dimension) const {
    auto bytes = field_.binary_value();
    if (!bytes.has_value()) {
        return err(LuceneError::illegal_state("the range field holds no bytes"));
    }
    if (dimension >= field_.field_type().point_dimension_count() / 2) {
        return err(LuceneError::illegal_argument("dimension out of range"));
    }
    return decode_max(bytes.value(), dimension);
}

VoidResult IntRange::set_range_values(std::span<const int32_t> min, std::span<const int32_t> max) {
    if (auto checked = check_args(min, max); !checked.has_value()) {
        return checked;
    }
    const size_t dimensions = field_.field_type().point_dimension_count() / 2;
    if (min.size() != dimensions || max.size() != dimensions) {
        return err(LuceneError::illegal_argument(
                "field (name=" + std::string(field_.name()) + ") uses " + std::to_string(dimensions) +
                " dimensions; cannot change to (incoming) " + std::to_string(min.size()) + " dimensions"));
    }
    auto encoded = encode(min, max);
    if (!encoded.has_value()) {
        return std::unexpected(std::move(encoded.error()));
    }
    return field_.set_bytes_value(std::move(encoded.value()));
}

Result<RangeFieldQuery> IntRange::new_relation_query(std::string field, std::span<const int32_t> min,
                                                     std::span<const int32_t> max, QueryType relation) {
    if (auto checked = check_args(min, max); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    auto ranges = encode(min, max);
    if (!ranges.has_value()) {
        return std::unexpected(std::move(ranges.error()));
    }
    return RangeFieldQuery::create(std::move(field), std::move(ranges.value()), min.size(), relation,
                                   int_range_format());
}

Result<RangeFieldQuery> IntRange::new_intersects_query(std::string field, std::span<const int32_t> min,
                                                       std::span<const int32_t> max) {
    return new_relation_query(std::move(field), min, max, QueryType::Intersects);
}

Result<RangeFieldQuery> IntRange::new_contains_query(std::string field, std::span<const int32_t> min,
                                                     std::span<const int32_t> max) {
    return new_relation_query(std::move(field), min, max, QueryType::Contains);
}

Result<RangeFieldQuery> IntRange::new_within_query(std::string field, std::span<const int32_t> min,
                                                   std::span<const int32_t> max) {
    return new_relation_query(std::move(field), min, max, QueryType::Within);
}

Result<RangeFieldQuery> IntRange::new_crosses_query(std::string field, std::span<const int32_t> min,
                                                    std::span<const int32_t> max) {
    return new_relation_query(std::move(field), min, max, QueryType::Crosses);
}

std::string IntRange::to_string() const {
    std::string result = "IntRange <" + std::string(field_.name()) + ": ";
    auto bytes = field_.binary_value();
    if (!bytes.has_value()) {
        return result + "Unsupported range data>";
    }
    const size_t dimensions = field_.field_type().point_dimension_count() / 2;
    for (size_t dim = 0; dim < dimensions; ++dim) {
        if (dim > 0) {
            result += ' ';
        }
        auto min = decode_min(bytes.value(), dim);
        auto max = decode_max(bytes.value(), dim);
        if (!min.has_value() || !max.has_value()) {
            return result + "Unsupported range data>";
        }
        result += std::format("[{} : {}]", min.value(), max.value());
    }
    result += '>';
    return result;
}


namespace {

/// Renders `min`/`max` the way the slow range queries describe themselves.
[[nodiscard]] std::string int_range_range_text(std::span<const int32_t> min, std::span<const int32_t> max) {
    const auto join = [](std::span<const int32_t> values) {
        std::string text = "[";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                text += ", ";
            }
            text += std::format("{}", values[i]);
        }
        text += "]";
        return text;
    };
    return "[" + join(min) + " TO " + join(max) + "]";
}

} // namespace

Result<Field> IntRangeDocValuesField::create(std::string name, std::span<const int32_t> min,
                                          std::span<const int32_t> max) {
    auto bytes = IntRange::encode(min, max);
    if (!bytes.has_value()) {
        return std::unexpected(std::move(bytes.error()));
    }
    FieldType type;
    std::ignore = type.set_doc_values_type(DocValuesType::Binary);
    type.freeze();
    return Field::from_bytes(std::move(name), std::move(bytes.value()), std::move(type));
}

VoidResult IntRangeDocValuesField::set_range_values(Field &field, std::span<const int32_t> min,
                                                 std::span<const int32_t> max) {
    if (field.field_type().doc_values_type() != DocValuesType::Binary) {
        return err(LuceneError::illegal_argument("the field is not a IntRangeDocValuesField"));
    }
    auto current = field.binary_value();
    if (!current.has_value()) {
        return err(LuceneError::illegal_state("the field holds no bytes"));
    }
    const size_t dimensions = current.value().size() / (2 * IntRange::BYTES);
    if (min.size() != dimensions || max.size() != dimensions) {
        return err(LuceneError::illegal_argument(
                "field (name=" + std::string(field.name()) + ") uses " + std::to_string(dimensions) +
                " dimensions; cannot change to (incoming) " + std::to_string(min.size()) + " dimensions"));
    }
    auto encoded = IntRange::encode(min, max);
    if (!encoded.has_value()) {
        return std::unexpected(std::move(encoded.error()));
    }
    return field.set_bytes_value(std::move(encoded.value()));
}

Result<BinaryRangeFieldRangeQuery> IntRangeDocValuesField::new_slow_intersects_query(std::string field,
                                                                                  std::span<const int32_t> min,
                                                                                  std::span<const int32_t> max) {
    auto ranges = IntRange::encode(min, max);
    if (!ranges.has_value()) {
        return std::unexpected(std::move(ranges.error()));
    }
    return BinaryRangeFieldRangeQuery::create(std::move(field), std::move(ranges.value()), IntRange::BYTES,
                                              min.size(), QueryType::Intersects,
                                              int_range_range_text(min, max));
}

} // namespace lucene
