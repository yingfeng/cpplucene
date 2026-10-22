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

module lucene_core:sorted_doc_values_writer.impl;

import :sorted_doc_values_writer;

import :bytes_ref;
import :doc_id_set_iterator;
import :doc_values_consumer;
import :doc_values_producer;
import :doc_values_writer;
import :docs_with_field_set;
import :error;
import :field_infos;
import :packed_long_values;
import :sorted_doc_values;
import :types;

import std;

namespace lucene {

namespace {

constexpr int32_t BITS_PER_VALUE = static_cast<int32_t>(sizeof(int64_t) * 8);

[[nodiscard]] bool bytes_less(const BytesRef &left, const BytesRef &right) {
    return left.compare_to(right) < 0;
}

} // namespace

// ---------------------------------------------------------------------------
// BufferedSortedDocValues
// ---------------------------------------------------------------------------

BufferedSortedDocValues::BufferedSortedDocValues(std::shared_ptr<const std::vector<BytesRef>> terms,
                                                 std::shared_ptr<const PackedLongValues> ords,
                                                 std::unique_ptr<DocIdSetIterator> docs_with_field)
        : terms_(std::move(terms)),
          ords_(std::move(ords)),
          ords_iterator_(ords_->iterator()),
          docs_with_field_(std::move(docs_with_field)) {}

VoidResult BufferedSortedDocValues::read_ord() {
    auto ord = ords_iterator_->next();
    if (!ord.has_value()) {
        return std::unexpected(std::move(ord.error()));
    }
    ord_ = static_cast<int32_t>(ord.value());
    return {};
}

Result<int32_t> BufferedSortedDocValues::next_doc() {
    auto doc = docs_with_field_->next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        if (auto read = read_ord(); !read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
    }
    return doc.value();
}

Result<int32_t> BufferedSortedDocValues::advance(int32_t target) {
    auto doc = docs_with_field_->advance(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        if (auto read = read_ord(); !read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
    }
    return doc.value();
}

Result<int64_t> BufferedSortedDocValues::cost() const { return docs_with_field_->cost(); }

Result<bool> BufferedSortedDocValues::advance_exact(int32_t target) {
    auto doc = docs_with_field_->advance(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != target) {
        return false;
    }
    if (auto read = read_ord(); !read.has_value()) {
        return std::unexpected(std::move(read.error()));
    }
    return true;
}

Result<BytesRef> BufferedSortedDocValues::lookup_ord(int32_t ord) {
    if (ord < 0 || static_cast<size_t>(ord) >= terms_->size()) {
        return err(LuceneError::illegal_argument("ordinal " + std::to_string(ord) + " is out of bounds"));
    }
    return terms_->at(static_cast<size_t>(ord));
}

Result<int32_t> BufferedSortedDocValues::get_value_count() const {
    return static_cast<int32_t>(terms_->size());
}

// ---------------------------------------------------------------------------
// SortedDocValuesProducer
// ---------------------------------------------------------------------------

SortedDocValuesProducer::SortedDocValuesProducer(std::shared_ptr<FieldInfo> field_info,
                                                 std::shared_ptr<const std::vector<BytesRef>> terms,
                                                 std::shared_ptr<const PackedLongValues> ords,
                                                 const DocsWithFieldSet &docs_with_field)
        : field_info_(std::move(field_info)),
          terms_(std::move(terms)),
          ords_(std::move(ords)),
          docs_with_field_(&docs_with_field) {}

Result<std::unique_ptr<SortedDocValues>> SortedDocValuesProducer::get_sorted(const FieldInfo &field) const {
    if (field.name != field_info_->name) {
        return err(LuceneError::illegal_argument("wrong fieldInfo: expected \"" + field_info_->name + "\", got \"" +
                                                field.name + "\""));
    }
    auto docs = docs_with_field_->iterator();
    if (!docs.has_value()) {
        return std::unexpected(std::move(docs.error()));
    }
    return std::unique_ptr<SortedDocValues>(
            std::make_unique<BufferedSortedDocValues>(terms_, ords_, std::move(docs.value())));
}

// ---------------------------------------------------------------------------
// SortedDocValuesWriter
// ---------------------------------------------------------------------------

SortedDocValuesWriter::SortedDocValuesWriter(std::shared_ptr<FieldInfo> field_info)
        : field_info_(std::move(field_info)) {}

VoidResult SortedDocValuesWriter::add_value(int32_t doc_id, std::span<const uint8_t> value) {
    if (doc_id <= last_doc_id_) {
        return err(LuceneError::illegal_argument("DocValuesField \"" + field_info_->name +
                                                 "\" appears more than once in this document (only one value is "
                                                 "allowed per field)"));
    }
    pending_.emplace_back(doc_id, BytesRef(std::vector<uint8_t>(value.begin(), value.end())));
    if (auto added = docs_with_field_.add(doc_id); !added.has_value()) {
        return added;
    }
    last_doc_id_ = doc_id;
    return {};
}

VoidResult SortedDocValuesWriter::finish() {
    if (final_terms_ != nullptr) {
        return {};
    }

    // The dictionary: every buffered term, sorted and deduplicated.
    std::vector<BytesRef> terms;
    terms.reserve(pending_.size());
    for (const auto &[doc_id, value] : pending_) {
        (void)doc_id;
        terms.push_back(value);
    }
    std::sort(terms.begin(), terms.end(), bytes_less);
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());

    // Then the ordinal of every buffered document, in document order.
    PackedLongValues::Builder ords(PackedLongValues::DEFAULT_PAGE_SIZE, BITS_PER_VALUE, 0, true);
    for (const auto &[doc_id, value] : pending_) {
        (void)doc_id;
        const auto found = std::lower_bound(terms.begin(), terms.end(), value, bytes_less);
        if (found == terms.end() || !(*found == value)) {
            return err(LuceneError::illegal_state("the buffered term is missing from the dictionary"));
        }
        if (auto added = ords.add(static_cast<int64_t>(std::distance(terms.begin(), found))); !added.has_value()) {
            return added;
        }
    }

    auto built = ords.build();
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    final_terms_ = std::make_shared<const std::vector<BytesRef>>(std::move(terms));
    final_ords_ = std::make_shared<const PackedLongValues>(std::move(built.value()));
    return docs_with_field_.finish();
}

VoidResult SortedDocValuesWriter::flush(DocValuesConsumer &consumer) {
    if (final_terms_ == nullptr) {
        if (auto finished = finish(); !finished.has_value()) {
            return finished;
        }
    }
    SortedDocValuesProducer producer(field_info_, final_terms_, final_ords_, docs_with_field_);
    return consumer.add_sorted_field(*field_info_, producer);
}

} // namespace lucene
