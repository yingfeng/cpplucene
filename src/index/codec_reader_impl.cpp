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

module lucene_core:codec_reader.impl;

import :codec_reader;

import :binary_doc_values;
import :core_helper;
import :doc_values_skipper;
import :document;
import :error;
import :field_infos;
import :fields;
import :index_types;
import :numeric_doc_values;
import :point_values;
import :points_reader;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :singleton_sorted_set_doc_values;
import :sorted_set_doc_values;
import :stored_field_visitor;
import :stored_fields;
import :terms;
import :types;

import std;

namespace lucene {

namespace {

/// The bounds-checking wrapper of the reference implementation's `StoredFieldsImpl`.
///
/// The codec readers are not required to validate document ids, so the check is
/// done here, before the id reaches the file format.
class CheckedStoredFields final : public StoredFields {
public:
    CheckedStoredFields(std::unique_ptr<StoredFields> inner, int32_t max_doc)
            : inner_(std::move(inner)), max_doc_(max_doc) {}

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override {
        if (auto checked = check(doc_id); !checked.has_value()) {
            return checked;
        }
        return inner_->prefetch(doc_id);
    }

    [[nodiscard]] VoidResult document_with_visitor(int32_t doc_id, StoredFieldVisitor &visitor) override {
        if (auto checked = check(doc_id); !checked.has_value()) {
            return checked;
        }
        return inner_->document_with_visitor(doc_id, visitor);
    }

    [[nodiscard]] Result<Document> document(int32_t doc_id) override {
        if (auto checked = check(doc_id); !checked.has_value()) {
            return std::unexpected(std::move(checked.error()));
        }
        return inner_->document(doc_id);
    }

private:
    [[nodiscard]] VoidResult check(int32_t doc_id) const {
        if (auto checked = CoreHelper::check_index(static_cast<size_t>(doc_id), static_cast<size_t>(max_doc_));
            !checked.has_value()) {
            return std::unexpected(std::move(checked.error()));
        }
        return {};
    }

    std::unique_ptr<StoredFields> inner_;
    int32_t max_doc_;
};

/// The `OptionalTermVectors::Empty` of the reference implementation: a cursor that owns no files and
/// answers "this document has no term vectors" for every document.
class EmptyTermVectors final : public TermVectors {
public:
    [[nodiscard]] Result<std::unique_ptr<Fields>> get(int32_t /*doc*/) override {
        return std::unique_ptr<Fields>{};
    }

    [[nodiscard]] Result<std::unique_ptr<Terms>> get_field_terms(int32_t /*doc*/, std::string_view /*field*/) override {
        return std::unique_ptr<Terms>{};
    }
};

} // namespace

Result<std::unique_ptr<StoredFields>> CodecReader::stored_fields() {
    auto *reader = get_fields_reader();
    if (reader == nullptr) {
        return std::unexpected(LuceneError::illegal_state("stored fields reader is null"));
    }
    auto max = max_doc();
    if (!max.has_value()) {
        return std::unexpected(std::move(max.error()));
    }
    auto clone = reader->clone();
    if (!clone.has_value()) {
        return std::unexpected(std::move(clone.error()));
    }
    return std::unique_ptr<StoredFields>(new CheckedStoredFields(std::move(clone.value()), max.value()));
}

Result<std::unique_ptr<TermVectors>> CodecReader::term_vectors() {
    auto *reader = get_term_vectors_reader();
    if (reader == nullptr) {
        return std::unique_ptr<TermVectors>(new EmptyTermVectors());
    }
    return reader->clone();
}

Result<std::unique_ptr<Terms>> CodecReader::terms(std::string_view field) {
    auto infos = field_infos();
    if (!infos.has_value()) {
        return std::unexpected(std::move(infos.error()));
    }
    const FieldInfo *fi = infos.value() == nullptr ? nullptr : infos.value()->by_name(field);
    if (fi == nullptr) {
        // Field does not exist.
        return std::unique_ptr<Terms>{};
    }
    if (fi->index_options == IndexOptions::None) {
        // Field exists but does not index postings.
        return std::unique_ptr<Terms>{};
    }
    auto *postings = get_postings_reader();
    if (postings == nullptr) {
        return std::unexpected(LuceneError::illegal_state("postings reader is null"));
    }
    return postings->terms(field);
}

Result<const FieldInfo *> CodecReader::doc_values_field(std::string_view field, DocValuesType expected) const {
    auto infos = field_infos();
    if (!infos.has_value()) {
        return std::unexpected(std::move(infos.error()));
    }
    const FieldInfo *fi = infos.value() == nullptr ? nullptr : infos.value()->by_name(field);
    if (fi == nullptr) {
        // Field does not exist.
        return static_cast<const FieldInfo *>(nullptr);
    }
    if (fi->doc_values_type == DocValuesType::None) {
        // Field was not indexed with doc values.
        return static_cast<const FieldInfo *>(nullptr);
    }
    if (fi->doc_values_type != expected) {
        // Field doc values are of a different type than requested.
        return static_cast<const FieldInfo *>(nullptr);
    }
    return fi;
}

Result<std::unique_ptr<NumericDocValues>> CodecReader::get_numeric_doc_values(std::string_view field) {
    auto fi = doc_values_field(field, DocValuesType::Numeric);
    if (!fi.has_value()) {
        return std::unexpected(std::move(fi.error()));
    }
    if (fi.value() == nullptr) {
        return std::unique_ptr<NumericDocValues>{};
    }
    auto *reader = get_doc_values_reader();
    if (reader == nullptr) {
        return std::unexpected(LuceneError::illegal_state("doc values reader is null"));
    }
    return reader->get_numeric(*fi.value());
}

Result<std::unique_ptr<BinaryDocValues>> CodecReader::get_binary_doc_values(std::string_view field) {
    auto fi = doc_values_field(field, DocValuesType::Binary);
    if (!fi.has_value()) {
        return std::unexpected(std::move(fi.error()));
    }
    if (fi.value() == nullptr) {
        return std::unique_ptr<BinaryDocValues>{};
    }
    auto *reader = get_doc_values_reader();
    if (reader == nullptr) {
        return std::unexpected(LuceneError::illegal_state("doc values reader is null"));
    }
    return reader->get_binary(*fi.value());
}

Result<std::unique_ptr<SortedDocValues>> CodecReader::get_sorted_doc_values(std::string_view field) {
    auto fi = doc_values_field(field, DocValuesType::Sorted);
    if (!fi.has_value()) {
        return std::unexpected(std::move(fi.error()));
    }
    if (fi.value() == nullptr) {
        return std::unique_ptr<SortedDocValues>{};
    }
    auto *reader = get_doc_values_reader();
    if (reader == nullptr) {
        return std::unexpected(LuceneError::illegal_state("doc values reader is null"));
    }
    return reader->get_sorted(*fi.value());
}

Result<std::unique_ptr<SortedNumericDocValues>> CodecReader::get_sorted_numeric_doc_values(std::string_view field) {
    auto fi = doc_values_field(field, DocValuesType::SortedNumeric);
    if (!fi.has_value()) {
        return std::unexpected(std::move(fi.error()));
    }
    if (fi.value() == nullptr) {
        return std::unique_ptr<SortedNumericDocValues>{};
    }
    auto *reader = get_doc_values_reader();
    if (reader == nullptr) {
        return std::unexpected(LuceneError::illegal_state("doc values reader is null"));
    }
    return reader->get_sorted_numeric(*fi.value());
}

Result<std::unique_ptr<SortedSetDocValues>> CodecReader::get_sorted_set_doc_values(std::string_view field) {
    auto fi = doc_values_field(field, DocValuesType::SortedSet);
    if (!fi.has_value()) {
        return std::unexpected(std::move(fi.error()));
    }
    auto *reader = get_doc_values_reader();
    if (fi.value() != nullptr) {
        if (reader == nullptr) {
            return std::unexpected(LuceneError::illegal_state("doc values reader is null"));
        }
        return reader->get_sorted_set(*fi.value());
    }

    // A single-valued field is exposed as a sorted set that holds exactly one value
    // per document: the reference implementation wraps the sorted cursor in
    // `SingletonSortedSetDocValues` the same way.
    auto single = doc_values_field(field, DocValuesType::Sorted);
    if (!single.has_value()) {
        return std::unexpected(std::move(single.error()));
    }
    if (single.value() == nullptr) {
        return std::unique_ptr<SortedSetDocValues>{};
    }
    if (reader == nullptr) {
        return std::unexpected(LuceneError::illegal_state("doc values reader is null"));
    }
    auto values = reader->get_sorted(*single.value());
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    return std::unique_ptr<SortedSetDocValues>(std::make_unique<SingletonSortedSetDocValues>(std::move(values.value())));
}

Result<std::unique_ptr<DocValuesSkipper>> CodecReader::get_doc_values_skipper(std::string_view field) {
    auto infos = field_infos();
    if (!infos.has_value()) {
        return std::unexpected(std::move(infos.error()));
    }
    const FieldInfo *fi = infos.value() == nullptr ? nullptr : infos.value()->by_name(field);
    if (fi == nullptr || fi->doc_values_skip_index == DocValuesSkipIndexType::None) {
        return std::unique_ptr<DocValuesSkipper>{};
    }
    auto *reader = get_doc_values_reader();
    if (reader == nullptr) {
        return std::unexpected(LuceneError::illegal_state("doc values reader is null"));
    }
    return reader->get_skipper(*fi);
}

Result<std::unique_ptr<NumericDocValues>> CodecReader::get_norm_values(std::string_view field) {
    auto infos = field_infos();
    if (!infos.has_value()) {
        return std::unexpected(std::move(infos.error()));
    }
    const FieldInfo *fi = infos.value() == nullptr ? nullptr : infos.value()->by_name(field);
    if (fi == nullptr || !fi->has_norms()) {
        return std::unique_ptr<NumericDocValues>{};
    }
    auto *reader = get_norms_reader();
    if (reader == nullptr) {
        return std::unexpected(LuceneError::illegal_state("norms reader is null"));
    }
    return reader->get_norms(*fi);
}

Result<std::shared_ptr<PointValues>> CodecReader::point_values(std::string_view field) {
    auto infos = field_infos();
    if (!infos.has_value()) {
        return std::unexpected(std::move(infos.error()));
    }
    const FieldInfo *fi = infos.value() == nullptr ? nullptr : infos.value()->by_name(field);
    if (fi == nullptr || fi->point_dimension_count == 0) {
        return std::shared_ptr<PointValues>{};
    }
    auto *reader = get_points_reader();
    if (reader == nullptr) {
        return std::unexpected(LuceneError::illegal_state("points reader is null"));
    }
    return reader->get_values(field);
}

VoidResult CodecReader::check_integrity() {
    // Terms/postings.
    if (auto *postings = get_postings_reader(); postings != nullptr) {
        if (auto checked = postings->check_integrity(); !checked.has_value()) {
            return checked;
        }
    }
    // Norms.
    if (auto *norms = get_norms_reader(); norms != nullptr) {
        if (auto checked = norms->check_integrity(); !checked.has_value()) {
            return checked;
        }
    }
    // Doc values.
    if (auto *doc_values = get_doc_values_reader(); doc_values != nullptr) {
        if (auto checked = doc_values->check_integrity(); !checked.has_value()) {
            return checked;
        }
    }
    // Stored fields.
    if (auto *fields = get_fields_reader(); fields != nullptr) {
        if (auto checked = fields->check_integrity(); !checked.has_value()) {
            return checked;
        }
    }
    // Term vectors.
    if (auto *vectors = get_term_vectors_reader(); vectors != nullptr) {
        if (auto checked = vectors->check_integrity(); !checked.has_value()) {
            return checked;
        }
    }
    // Points.
    if (auto *points = get_points_reader(); points != nullptr) {
        if (auto checked = points->check_integrity(); !checked.has_value()) {
            return checked;
        }
    }
    return {};
}

} // namespace lucene
