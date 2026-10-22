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

import :analyzer;
import :field_token_streams;
import :field_type;
import :invertable_field;
import :token_stream;

namespace lucene {

/// A single field instance of a document.  Port of `IndexableField`.
///
/// The reference implementation models the payload as an enum that also covers token streams and
/// readers.  This port keeps the value in the field and hands out a token
/// stream on demand ([`token_stream`]), which is what the indexing chain needs.
export class IndexableField {
public:
    virtual ~IndexableField() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual const FieldType &field_type() const = 0;

    [[nodiscard]] virtual std::optional<std::string_view> string_value() const = 0;
    [[nodiscard]] virtual std::optional<std::vector<uint8_t>> binary_value() const = 0;
    [[nodiscard]] virtual std::optional<Number> numeric_value() const = 0;

    /// How the field must be inverted (port of `IndexableField::invertable_type`).
    [[nodiscard]] virtual InvertableType invertable_type() const { return InvertableType::TokenStream; }

    /// Whether the indexer owns this field and refuses it in user documents
    /// (the soft deletes field, the parent field of a block index).
    [[nodiscard]] virtual bool is_reserved() const { return false; }

    /// The token stream used to index this field.
    ///
    /// `nullptr` means "this field is not indexed".  The stream stays valid
    /// until the next call with the same field name, so the indexer must finish
    /// one field before starting the next (which is why the indexing chain has
    /// two passes over the document).
    ///
    /// **Deviation:** the reference implementation passes the reusable stream in and out; here the
    /// field (or its analyzer) owns it and `const` is enough.
    [[nodiscard]] virtual Result<TokenStream *> token_stream(Analyzer &analyzer) const {
        return err(LuceneError::unsupported_operation("this field cannot produce a token stream"));
    }

    [[nodiscard]] virtual std::string to_string() const = 0;
};

/// The value carried by a [`Field`].
export using FieldValue = std::variant<std::string, std::vector<uint8_t>, Number>;

/// The concrete [`IndexableField`].  Port of `document/field.rs`.
export class Field : public IndexableField {
public:
    Field(std::string name, FieldValue value, FieldType type);

    // The cached token streams are derived state, so copies start without them.
    Field(const Field &other);
    Field &operator=(const Field &other);
    Field(Field &&other) noexcept;
    Field &operator=(Field &&other) noexcept;
    ~Field() override = default;

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

    [[nodiscard]] Result<TokenStream *> token_stream(Analyzer &analyzer) const override;

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
    /// Token streams for the non-tokenized cases, reused across documents.
    /// They are derived state: copies start with none.
    mutable std::unique_ptr<StringTokenStream> string_stream_;
    mutable std::unique_ptr<BinaryTokenStream> binary_stream_;
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
