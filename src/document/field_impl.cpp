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

module lucene_core:indexable_field.impl;

import :indexable_field;
import :types;
import :error;
import :number;
import :field_type;

import std;

namespace lucene {

Field::Field(std::string name, FieldValue value, FieldType type)
    : name_(std::move(name)), value_(std::move(value)), type_(std::move(type)) {}

Result<Field> Field::from_string(std::string name, std::string value, FieldType type) {
    if (auto r = type.validate(); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return Field(std::move(name), FieldValue{std::move(value)}, std::move(type));
}

Result<Field> Field::from_bytes(std::string name, std::vector<uint8_t> value, FieldType type) {
    if (auto r = type.validate(); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return Field(std::move(name), FieldValue{std::move(value)}, std::move(type));
}

Result<Field> Field::from_i32(std::string name, int32_t value, FieldType type) {
    if (auto r = type.validate(); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return Field(std::move(name), FieldValue{Number{value}}, std::move(type));
}

Result<Field> Field::from_i64(std::string name, int64_t value, FieldType type) {
    if (auto r = type.validate(); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return Field(std::move(name), FieldValue{Number{value}}, std::move(type));
}

Result<Field> Field::from_f32(std::string name, float value, FieldType type) {
    if (auto r = type.validate(); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return Field(std::move(name), FieldValue{Number{value}}, std::move(type));
}

Result<Field> Field::from_f64(std::string name, double value, FieldType type) {
    if (auto r = type.validate(); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return Field(std::move(name), FieldValue{Number{value}}, std::move(type));
}

std::optional<std::string_view> Field::string_value() const {
    if (const auto *text = std::get_if<std::string>(&value_); text != nullptr) {
        return std::string_view(*text);
    }
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> Field::binary_value() const {
    if (const auto *bytes = std::get_if<std::vector<uint8_t>>(&value_); bytes != nullptr) {
        return *bytes;
    }
    return std::nullopt;
}

std::optional<Number> Field::numeric_value() const {
    if (const auto *number = std::get_if<Number>(&value_); number != nullptr) {
        return *number;
    }
    return std::nullopt;
}

std::string Field::to_string() const {
    std::string result = std::format("{}<{}:", name_, type_.to_string());
    if (const auto *text = std::get_if<std::string>(&value_); text != nullptr) {
        result += *text;
    } else if (const auto *bytes = std::get_if<std::vector<uint8_t>>(&value_); bytes != nullptr) {
        static constexpr char kHex[] = "0123456789abcdef";
        for (const uint8_t byte : *bytes) {
            result.push_back(kHex[byte >> 4]);
            result.push_back(kHex[byte & 0x0F]);
        }
    } else if (const auto *number = std::get_if<Number>(&value_); number != nullptr) {
        result += number_to_string(*number);
    }
    result.push_back('>');
    return result;
}

VoidResult Field::set_string_value(std::string value) {
    if (std::holds_alternative<std::vector<uint8_t>>(value_)) {
        return err(LuceneError::illegal_argument("cannot change a binary field to a string field"));
    }
    value_ = FieldValue{std::move(value)};
    return {};
}

VoidResult Field::set_bytes_value(std::vector<uint8_t> value) {
    if (std::holds_alternative<std::string>(value_)) {
        return err(LuceneError::illegal_argument("cannot change a string field to a binary field"));
    }
    value_ = FieldValue{std::move(value)};
    return {};
}

VoidResult Field::set_i32_value(int32_t value) {
    value_ = FieldValue{Number{value}};
    return {};
}

VoidResult Field::set_i64_value(int64_t value) {
    value_ = FieldValue{Number{value}};
    return {};
}

VoidResult Field::set_f32_value(float value) {
    value_ = FieldValue{Number{value}};
    return {};
}

VoidResult Field::set_f64_value(double value) {
    value_ = FieldValue{Number{value}};
    return {};
}

// ---------------- StoredField ----------------

FieldType StoredField::stored_type() {
    FieldType type;
    std::ignore = type.set_stored(true);
    std::ignore = type.set_tokenized(false);
    type.freeze();
    return type;
}

Field StoredField::from_string(std::string name, std::string value) {
    return Field(std::move(name), FieldValue{std::move(value)}, stored_type());
}

Field StoredField::from_bytes(std::string name, std::vector<uint8_t> value) {
    return Field(std::move(name), FieldValue{std::move(value)}, stored_type());
}

Field StoredField::from_i32(std::string name, int32_t value) {
    return Field(std::move(name), FieldValue{Number{value}}, stored_type());
}

Field StoredField::from_i64(std::string name, int64_t value) {
    return Field(std::move(name), FieldValue{Number{value}}, stored_type());
}

Field StoredField::from_f32(std::string name, float value) {
    return Field(std::move(name), FieldValue{Number{value}}, stored_type());
}

Field StoredField::from_f64(std::string name, double value) {
    return Field(std::move(name), FieldValue{Number{value}}, stored_type());
}

} // namespace lucene
