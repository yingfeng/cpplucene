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

module lucene_core:sorted_set_doc_values_writer.impl;

import :sorted_set_doc_values_writer;

import :bytes_ref;
import :debug;
import :doc_id_set_iterator;
import :doc_values_consumer;
import :doc_values_producer;
import :doc_values_writer;
import :docs_with_field_set;
import :error;
import :field_infos;
import :packed_long_values;
import :sorted_set_doc_values;
import :types;

import std;

namespace lucene {

namespace {

constexpr int32_t BITS_PER_VALUE = static_cast<int32_t>(sizeof(int64_t) * 8);

[[nodiscard]] bool bytes_less(const BytesRef &left, const BytesRef &right) {
    return left.compare_to(right) < 0;
}

[[nodiscard]] PackedLongValues::Builder delta_builder() {
    return PackedLongValues::Builder(PackedLongValues::DEFAULT_PAGE_SIZE, BITS_PER_VALUE, 0, true);
}

} // namespace

// ---------------------------------------------------------------------------
// BufferedSortedSetDocValues
// ---------------------------------------------------------------------------

BufferedSortedSetDocValues::BufferedSortedSetDocValues(std::shared_ptr<const std::vector<BytesRef>> terms,
                                                       std::shared_ptr<const PackedLongValues> ords,
                                                       std::shared_ptr<const PackedLongValues> value_counts,
                                                       std::unique_ptr<DocIdSetIterator> docs_with_field)
        : terms_(std::move(terms)),
          ords_(std::move(ords)),
          value_counts_(std::move(value_counts)),
          ords_iterator_(ords_->iterator()),
          counts_iterator_(value_counts_->iterator()),
          docs_with_field_(std::move(docs_with_field)) {}

VoidResult BufferedSortedSetDocValues::read_count() {
    auto count = counts_iterator_->next();
    if (!count.has_value()) {
        return std::unexpected(std::move(count.error()));
    }
    count_ = static_cast<int32_t>(count.value());
    upto_ = 0;
    return {};
}

Result<int32_t> BufferedSortedSetDocValues::next_doc() {
    auto doc = docs_with_field_->next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        if (auto read = read_count(); !read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
    }
    return doc.value();
}

Result<int32_t> BufferedSortedSetDocValues::advance(int32_t target) {
    auto doc = docs_with_field_->advance(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        if (auto read = read_count(); !read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
    }
    return doc.value();
}

Result<int64_t> BufferedSortedSetDocValues::cost() const { return docs_with_field_->cost(); }

Result<bool> BufferedSortedSetDocValues::advance_exact(int32_t target) {
    auto doc = docs_with_field_->advance(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != target) {
        return false;
    }
    if (auto read = read_count(); !read.has_value()) {
        return std::unexpected(std::move(read.error()));
    }
    return true;
}

Result<int64_t> BufferedSortedSetDocValues::next_ord() {
    if (upto_ >= count_) {
        return NO_MORE_ORDS;
    }
    upto_ += 1;
    return ords_iterator_->next();
}

Result<BytesRef> BufferedSortedSetDocValues::lookup_ord(int64_t ord) {
    if (ord < 0 || static_cast<size_t>(ord) >= terms_->size()) {
        return err(LuceneError::illegal_argument("ordinal " + std::to_string(ord) + " is out of bounds"));
    }
    return terms_->at(static_cast<size_t>(ord));
}

Result<int64_t> BufferedSortedSetDocValues::get_value_count() const {
    return static_cast<int64_t>(terms_->size());
}

// ---------------------------------------------------------------------------
// SortedSetDocValuesProducer
// ---------------------------------------------------------------------------

SortedSetDocValuesProducer::SortedSetDocValuesProducer(
        std::shared_ptr<FieldInfo> field_info, std::shared_ptr<const std::vector<BytesRef>> terms,
        std::shared_ptr<const PackedLongValues> ords, std::shared_ptr<const PackedLongValues> value_counts,
        const DocsWithFieldSet &docs_with_field)
        : field_info_(std::move(field_info)),
          terms_(std::move(terms)),
          ords_(std::move(ords)),
          value_counts_(std::move(value_counts)),
          docs_with_field_(&docs_with_field) {}

Result<std::unique_ptr<SortedSetDocValues>> SortedSetDocValuesProducer::get_sorted_set(
        const FieldInfo &field) const {
    if (field.name != field_info_->name) {
        return err(LuceneError::illegal_argument("wrong fieldInfo: expected \"" + field_info_->name + "\", got \"" +
                                                field.name + "\""));
    }
    auto docs = docs_with_field_->iterator();
    if (!docs.has_value()) {
        return std::unexpected(std::move(docs.error()));
    }
    return std::unique_ptr<SortedSetDocValues>(std::make_unique<BufferedSortedSetDocValues>(
            terms_, ords_, value_counts_, std::move(docs.value())));
}

// ---------------------------------------------------------------------------
// SortedSetDocValuesWriter
// ---------------------------------------------------------------------------

SortedSetDocValuesWriter::SortedSetDocValuesWriter(std::shared_ptr<FieldInfo> field_info)
        : field_info_(std::move(field_info)) {}

VoidResult SortedSetDocValuesWriter::add_value(int32_t doc_id, std::span<const uint8_t> value) {
    debug_check(doc_id >= current_doc_, "the document ids must not go backwards");
    if (doc_id != current_doc_) {
        if (auto added = docs_with_field_.add(doc_id); !added.has_value()) {
            return added;
        }
        current_doc_ = doc_id;
    }
    pending_.emplace_back(doc_id, BytesRef(std::vector<uint8_t>(value.begin(), value.end())));
    return {};
}

VoidResult SortedSetDocValuesWriter::finish() {
    if (final_terms_ != nullptr) {
        return {};
    }

    // The dictionary: every distinct value, sorted.
    std::vector<BytesRef> terms;
    terms.reserve(pending_.size());
    for (const auto &[doc_id, value] : pending_) {
        (void)doc_id;
        terms.push_back(value);
    }
    std::sort(terms.begin(), terms.end(), bytes_less);
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());

    // Then one ascending ordinal run per document.
    auto ords = delta_builder();
    auto counts = delta_builder();
    std::vector<int32_t> current;
    size_t pos = 0;
    while (pos < pending_.size()) {
        const auto doc_id = pending_[pos].first;
        current.clear();
        while (pos < pending_.size() && pending_[pos].first == doc_id) {
            const auto &value = pending_[pos].second;
            const auto found = std::lower_bound(terms.begin(), terms.end(), value, bytes_less);
            if (found == terms.end() || !(*found == value)) {
                return err(LuceneError::illegal_state("the buffered term is missing from the dictionary"));
            }
            current.push_back(static_cast<int32_t>(std::distance(terms.begin(), found)));
            pos += 1;
        }
        // A sorted set has no duplicate ordinals and lists them in order.
        std::sort(current.begin(), current.end());
        current.erase(std::unique(current.begin(), current.end()), current.end());
        for (const auto ord : current) {
            if (auto added = ords.add(ord); !added.has_value()) {
                return added;
            }
        }
        if (auto added = counts.add(static_cast<int64_t>(current.size())); !added.has_value()) {
            return added;
        }
    }

    auto built_ords = ords.build();
    if (!built_ords.has_value()) {
        return std::unexpected(std::move(built_ords.error()));
    }
    auto built_counts = counts.build();
    if (!built_counts.has_value()) {
        return std::unexpected(std::move(built_counts.error()));
    }

    final_terms_ = std::make_shared<const std::vector<BytesRef>>(std::move(terms));
    final_ords_ = std::make_shared<const PackedLongValues>(std::move(built_ords.value()));
    final_value_counts_ = std::make_shared<const PackedLongValues>(std::move(built_counts.value()));
    return docs_with_field_.finish();
}

VoidResult SortedSetDocValuesWriter::flush(DocValuesConsumer &consumer) {
    if (final_terms_ == nullptr) {
        if (auto finished = finish(); !finished.has_value()) {
            return finished;
        }
    }
    SortedSetDocValuesProducer producer(field_info_, final_terms_, final_ords_, final_value_counts_,
                                        docs_with_field_);
    return consumer.add_sorted_set_field(*field_info_, producer);
}

} // namespace lucene
