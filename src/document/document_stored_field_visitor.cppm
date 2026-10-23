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

export module lucene_core:document_stored_field_visitor;

import :types;
import std;

import :document;
import :error;
import :field_infos;
import :indexable_field;
import :stored_field_visitor;

namespace lucene {

/// A [`StoredFieldVisitor`] that builds a [`Document`] out of the stored fields.
///
/// Port of `DocumentStoredFieldVisitor`
/// (`core/document/document_stored_field_visitor.rs`).
///
/// Loading every field is the default; constructing the visitor with a set of
/// names loads only those, which is what
/// [`StoredFields::document_with_visitor`] uses to keep a "select a few fields"
/// read cheap.
///
/// Note that only the stored information of the resulting fields is valid: the
/// indexing options, term vector options, and so on are set from the field info
/// for a string field and left unset otherwise.
export class DocumentStoredFieldVisitor final : public StoredFieldVisitor {
public:
    /// Loads every stored field.
    DocumentStoredFieldVisitor() = default;

    /// Loads only the stored fields whose name is in `fields_to_add`.
    explicit DocumentStoredFieldVisitor(std::set<std::string> fields_to_add);

    // ---- StoredFieldVisitor ----

    [[nodiscard]] Status needs_field(const FieldInfo &field_info) override;

    [[nodiscard]] VoidResult binary_field(const FieldInfo &field_info, std::span<const uint8_t> value) override;
    [[nodiscard]] VoidResult string_field(const FieldInfo &field_info, std::string_view value) override;
    [[nodiscard]] VoidResult int_field(const FieldInfo &field_info, int32_t value) override;
    [[nodiscard]] VoidResult long_field(const FieldInfo &field_info, int64_t value) override;
    [[nodiscard]] VoidResult float_field(const FieldInfo &field_info, float value) override;
    [[nodiscard]] VoidResult double_field(const FieldInfo &field_info, double value) override;

    /// The document built so far.
    [[nodiscard]] const Document &document() const noexcept { return document_; }

    /// The document built so far, moved out of the visitor.
    [[nodiscard]] Document take_document();

private:
    [[nodiscard]] VoidResult add(Field field);

    Document document_;
    std::optional<std::set<std::string>> fields_to_add_;
};

} // namespace lucene
