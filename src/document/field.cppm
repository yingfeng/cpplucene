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

export module lucene_core:indexable_field;

import :types;
import :error;
import :number;

import std;

import :field_type;

namespace lucene {

/// A single field instance of a document.  Port of `IndexableField`.
///
/// rlucene models the payload as an enum that also covers token streams and
/// readers.  The token-stream variants belong to the analysis layer and are
/// added with it; what is here covers everything the codecs read back.
export class IndexableField {
public:
    virtual ~IndexableField() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual const FieldType &field_type() const = 0;

    [[nodiscard]] virtual std::optional<std::string_view> string_value() const = 0;
    [[nodiscard]] virtual std::optional<std::vector<uint8_t>> binary_value() const = 0;
    [[nodiscard]] virtual std::optional<Number> numeric_value() const = 0;

    [[nodiscard]] virtual std::string to_string() const = 0;
};

/// The value carried by a [`Field`].
export using FieldValue = std::variant<std::string, std::vector<uint8_t>, Number>;

/// The concrete [`IndexableField`].  Port of `document/field.rs`.
export class Field : public IndexableField {
public:
    Field(std::string name, FieldValue value, FieldType type);

    // ---- factories ----
    [[nodiscard]] static Result<Field> from_string(std::string name, std::string value, FieldType type);
    [[nodiscard]] static Result<Field> from_bytes(std::string name, std::vector<uint8_t> value, FieldType type);
    [[nodiscard]] static Result<Field> from_i32(std::string name, int32_t value, FieldType type);
    [[nodiscard]] static Result<Field> from_i64(std::string name, int64_t value, FieldType type);
    [[nodiscard]] static Result<Field> from_f32(std::string name, float value, FieldType type);
    [[nodiscard]] static Result<Field> from_f64(std::string name, double value, FieldType type);

    [[nodiscard]] std::string_view name() const override { return name_; }
    [[nodiscard]] const FieldType &field_type() const override { return type_; }

    [[nodiscard]] std::optional<std::string_view> string_value() const override;
    [[nodiscard]] std::optional<std::vector<uint8_t>> binary_value() const override;
    [[nodiscard]] std::optional<Number> numeric_value() const override;

    [[nodiscard]] std::string to_string() const override;

    // ---- value accessors / setters ----
    [[nodiscard]] const FieldValue &value() const noexcept { return value_; }
    void set_value(FieldValue value) noexcept { value_ = std::move(value); }
    [[nodiscard]] VoidResult set_string_value(std::string value);
    [[nodiscard]] VoidResult set_bytes_value(std::vector<uint8_t> value);
    [[nodiscard]] VoidResult set_i32_value(int32_t value);
    [[nodiscard]] VoidResult set_i64_value(int64_t value);
    [[nodiscard]] VoidResult set_f32_value(float value);
    [[nodiscard]] VoidResult set_f64_value(double value);

private:
    std::string name_;
    FieldValue value_;
    FieldType type_;
};

/// A field whose value is only stored, never indexed.  Port of `StoredField`.
export class StoredField : public Field {
public:
    [[nodiscard]] static Field from_string(std::string name, std::string value);
    [[nodiscard]] static Field from_bytes(std::string name, std::vector<uint8_t> value);
    [[nodiscard]] static Field from_i32(std::string name, int32_t value);
    [[nodiscard]] static Field from_i64(std::string name, int64_t value);
    [[nodiscard]] static Field from_f32(std::string name, float value);
    [[nodiscard]] static Field from_f64(std::string name, double value);

private:
    /// The stored-only field type shared by every instance.
    [[nodiscard]] static FieldType stored_type();
};

} // namespace lucene
