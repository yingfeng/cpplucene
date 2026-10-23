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

module lucene_core:sorting_field_readers.impl;

import :sorting_field_readers;

import std;

import :document;
import :error;
import :fields;
import :index_sorter;
import :stored_field_visitor;
import :stored_fields;
import :stored_fields_reader;
import :term_vectors;
import :term_vectors_reader;
import :terms;
import :types;

namespace lucene {

namespace {

/// The old document id that `doc_id` stands for, rejecting ids the sort dropped.
[[nodiscard]] Result<int32_t> to_old_doc(const IndexSortDocMap &doc_map, int32_t doc_id) {
    auto old_doc = doc_map.new_to_old(doc_id);
    if (!old_doc.has_value()) {
        return std::unexpected(std::move(old_doc.error()));
    }
    return old_doc.value();
}

} // namespace

// ---- SortingStoredFieldsReader ----

SortingStoredFieldsReader::SortingStoredFieldsReader(std::shared_ptr<StoredFieldsReader> reader,
                                                     std::shared_ptr<const IndexSortDocMap> doc_map)
        : reader_(std::move(reader)), doc_map_(std::move(doc_map)) {}

Result<std::unique_ptr<SortingStoredFieldsReader>> SortingStoredFieldsReader::create(
        std::shared_ptr<StoredFieldsReader> reader, std::shared_ptr<const IndexSortDocMap> doc_map) {
    if (reader == nullptr || doc_map == nullptr) {
        return err(LuceneError::illegal_argument("the reader and the doc map must not be null"));
    }
    return std::unique_ptr<SortingStoredFieldsReader>(
            new SortingStoredFieldsReader(std::move(reader), std::move(doc_map)));
}

Result<int32_t> SortingStoredFieldsReader::old_doc(int32_t doc_id) const {
    return to_old_doc(*doc_map_, doc_id);
}

VoidResult SortingStoredFieldsReader::prefetch(int32_t doc_id) {
    auto mapped = old_doc(doc_id);
    if (!mapped.has_value()) {
        return std::unexpected(std::move(mapped.error()));
    }
    return reader_->prefetch(mapped.value());
}

VoidResult SortingStoredFieldsReader::document_with_visitor(int32_t doc_id, StoredFieldVisitor &visitor) {
    auto mapped = old_doc(doc_id);
    if (!mapped.has_value()) {
        return std::unexpected(std::move(mapped.error()));
    }
    return reader_->document_with_visitor(mapped.value(), visitor);
}

Result<Document> SortingStoredFieldsReader::document(int32_t doc_id) {
    auto mapped = old_doc(doc_id);
    if (!mapped.has_value()) {
        return std::unexpected(std::move(mapped.error()));
    }
    return reader_->document(mapped.value());
}

VoidResult SortingStoredFieldsReader::check_integrity() const {
    return reader_->check_integrity();
}

Result<std::unique_ptr<StoredFieldsReader>> SortingStoredFieldsReader::get_merge_instance() const {
    auto instance = reader_->get_merge_instance();
    if (!instance.has_value()) {
        return std::unexpected(std::move(instance.error()));
    }
    if (instance.value() == nullptr) {
        return clone();
    }
    return create(std::shared_ptr<StoredFieldsReader>(std::move(instance.value())), doc_map_);
}

Result<std::unique_ptr<StoredFieldsReader>> SortingStoredFieldsReader::clone() const {
    auto copy = reader_->clone();
    if (!copy.has_value()) {
        return std::unexpected(std::move(copy.error()));
    }
    return create(std::shared_ptr<StoredFieldsReader>(std::move(copy.value())), doc_map_);
}

VoidResult SortingStoredFieldsReader::close() const {
    return reader_->close();
}

// ---- SortingTermVectorsReader ----

SortingTermVectorsReader::SortingTermVectorsReader(std::shared_ptr<TermVectorsReader> reader,
                                                   std::shared_ptr<const IndexSortDocMap> doc_map)
        : reader_(std::move(reader)), doc_map_(std::move(doc_map)) {}

Result<std::unique_ptr<SortingTermVectorsReader>> SortingTermVectorsReader::create(
        std::shared_ptr<TermVectorsReader> reader, std::shared_ptr<const IndexSortDocMap> doc_map) {
    if (reader == nullptr || doc_map == nullptr) {
        return err(LuceneError::illegal_argument("the reader and the doc map must not be null"));
    }
    return std::unique_ptr<SortingTermVectorsReader>(
            new SortingTermVectorsReader(std::move(reader), std::move(doc_map)));
}

Result<int32_t> SortingTermVectorsReader::old_doc(int32_t doc_id) const {
    return to_old_doc(*doc_map_, doc_id);
}

VoidResult SortingTermVectorsReader::prefetch(int32_t doc_id) {
    auto mapped = old_doc(doc_id);
    if (!mapped.has_value()) {
        return std::unexpected(std::move(mapped.error()));
    }
    return reader_->prefetch(mapped.value());
}

Result<std::unique_ptr<Fields>> SortingTermVectorsReader::get(int32_t doc) {
    auto mapped = old_doc(doc);
    if (!mapped.has_value()) {
        return std::unexpected(std::move(mapped.error()));
    }
    return reader_->get(mapped.value());
}

VoidResult SortingTermVectorsReader::check_integrity() const {
    return reader_->check_integrity();
}

Result<std::unique_ptr<TermVectorsReader>> SortingTermVectorsReader::get_merge_instance() const {
    auto instance = reader_->get_merge_instance();
    if (!instance.has_value()) {
        return std::unexpected(std::move(instance.error()));
    }
    if (instance.value() == nullptr) {
        return clone();
    }
    return create(std::shared_ptr<TermVectorsReader>(std::move(instance.value())), doc_map_);
}

Result<std::unique_ptr<TermVectorsReader>> SortingTermVectorsReader::clone() const {
    auto copy = reader_->clone();
    if (!copy.has_value()) {
        return std::unexpected(std::move(copy.error()));
    }
    return create(std::shared_ptr<TermVectorsReader>(std::move(copy.value())), doc_map_);
}

VoidResult SortingTermVectorsReader::close() const {
    return reader_->close();
}

} // namespace lucene
