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

module lucene_core:document_stored_field_visitor.impl;

import :document_stored_field_visitor;

import std;

import :document;
import :error;
import :field_infos;
import :field_type;
import :indexable_field;
import :stored_field_visitor;
import :types;

namespace lucene {

DocumentStoredFieldVisitor::DocumentStoredFieldVisitor(std::set<std::string> fields_to_add)
        : fields_to_add_(std::move(fields_to_add)) {}

Status DocumentStoredFieldVisitor::needs_field(const FieldInfo &field_info) {
    if (!fields_to_add_.has_value()) {
        return Status::Yes;
    }
    return fields_to_add_->contains(field_info.name) ? Status::Yes : Status::No;
}

VoidResult DocumentStoredFieldVisitor::binary_field(const FieldInfo &field_info, std::span<const uint8_t> value) {
    return add(StoredField::from_bytes(field_info.name, std::vector<uint8_t>(value.begin(), value.end())));
}

VoidResult DocumentStoredFieldVisitor::string_field(const FieldInfo &field_info, std::string_view value) {
    // A stored string keeps the indexing options of the field it came from, so a
    // document that is written back out again does not lose them.
    FieldType type;
    if (auto stored = type.set_stored(true); !stored.has_value()) {
        return stored;
    }
    if (auto tokenized = type.set_tokenized(true); !tokenized.has_value()) {
        return tokenized;
    }
    if (auto term_vectors = type.set_store_term_vectors(field_info.store_term_vector); !term_vectors.has_value()) {
        return term_vectors;
    }
    if (auto norms = type.set_omit_norms(field_info.omit_norms); !norms.has_value()) {
        return norms;
    }
    if (auto options = type.set_index_options(field_info.index_options); !options.has_value()) {
        return options;
    }
    type.freeze();
    auto field = Field::from_string(field_info.name, std::string(value), std::move(type));
    if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
    }
    return add(std::move(field.value()));
}

VoidResult DocumentStoredFieldVisitor::int_field(const FieldInfo &field_info, int32_t value) {
    return add(StoredField::from_i32(field_info.name, value));
}

VoidResult DocumentStoredFieldVisitor::long_field(const FieldInfo &field_info, int64_t value) {
    return add(StoredField::from_i64(field_info.name, value));
}

VoidResult DocumentStoredFieldVisitor::float_field(const FieldInfo &field_info, float value) {
    return add(StoredField::from_f32(field_info.name, value));
}

VoidResult DocumentStoredFieldVisitor::double_field(const FieldInfo &field_info, double value) {
    return add(StoredField::from_f64(field_info.name, value));
}

Document DocumentStoredFieldVisitor::take_document() {
    return std::move(document_);
}

VoidResult DocumentStoredFieldVisitor::add(Field field) {
    document_.add(std::make_unique<Field>(std::move(field)));
    return {};
}

} // namespace lucene
