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

module lucene_core:sorting_doc_values.impl;

import :sorting_doc_values;

import std;

import :binary_doc_values;
import :doc_id_set_iterator;
import :error;
import :index_sorter;
import :numeric_doc_values;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :types;

namespace lucene {

namespace {

/// Walks `values`, handing the caller `(new_doc, old_doc)` for every document
/// that has a value; the pairs come out sorted by the new doc id.
template <typename OnValue>
[[nodiscard]] VoidResult collect(DocIdSetIterator &values, const IndexSortDocMap &doc_map, OnValue on_value) {
    while (true) {
        auto old_doc = values.next_doc();
        if (!old_doc.has_value()) {
            return std::unexpected(std::move(old_doc.error()));
        }
        if (old_doc.value() == NO_MORE_DOCS) {
            return {};
        }
        auto new_doc = doc_map.old_to_new(old_doc.value());
        if (!new_doc.has_value()) {
            return std::unexpected(std::move(new_doc.error()));
        }
        if (new_doc.value() < 0) {
            // The document was deleted; it has no place in the sorted segment.
            continue;
        }
        if (auto recorded = on_value(new_doc.value(), old_doc.value()); !recorded.has_value()) {
            return recorded;
        }
    }
}

/// Sorts one materialized array by the new doc id it is keyed by.
template <typename T>
void sort_by_new_doc(std::vector<std::pair<int32_t, T>> &entries) {
    std::ranges::stable_sort(entries, [](const auto &left, const auto &right) { return left.first < right.first; });
}

/// Finds `target` among `docs[position..)`.
[[nodiscard]] std::optional<size_t> find_doc(const std::vector<int32_t> &docs, int32_t target, size_t position) {
    auto begin = docs.begin() + static_cast<std::ptrdiff_t>(position);
    auto found = std::lower_bound(begin, docs.end(), target);
    if (found == docs.end() || *found != target) {
        return std::nullopt;
    }
    return static_cast<size_t>(found - docs.begin());
}

} // namespace

Result<std::vector<std::pair<int32_t, int32_t>>> new_doc_to_old(DocIdSetIterator &values,
                                                               const IndexSortDocMap &doc_map) {
    std::vector<std::pair<int32_t, int32_t>> pairs;
    auto collected = collect(values, doc_map, [&pairs](int32_t new_doc, int32_t old_doc) {
        pairs.emplace_back(new_doc, old_doc);
        return VoidResult{};
    });
    if (!collected.has_value()) {
        return std::unexpected(std::move(collected.error()));
    }
    sort_by_new_doc(pairs);
    return pairs;
}

// ---- SortingNumericDocValues ----

SortingNumericDocValues::SortingNumericDocValues(std::vector<int32_t> new_docs, std::vector<int64_t> values)
        : new_docs_(std::move(new_docs)), values_(std::move(values)) {}

Result<std::unique_ptr<SortingNumericDocValues>> SortingNumericDocValues::create(
        std::unique_ptr<NumericDocValues> values, const IndexSortDocMap &doc_map) {
    if (values == nullptr) {
        return err(LuceneError::illegal_argument("the doc values must not be null"));
    }
    std::vector<std::pair<int32_t, int64_t>> entries;
    auto collected = collect(*values, doc_map, [&values, &entries](int32_t new_doc, int32_t /*old_doc*/) {
        auto value = values->long_value();
        if (!value.has_value()) {
            return VoidResult(std::unexpected(std::move(value.error())));
        }
        entries.emplace_back(new_doc, value.value());
        return VoidResult{};
    });
    if (!collected.has_value()) {
        return std::unexpected(std::move(collected.error()));
    }
    sort_by_new_doc(entries);
    std::vector<int32_t> new_docs;
    std::vector<int64_t> materialized;
    new_docs.reserve(entries.size());
    materialized.reserve(entries.size());
    for (auto &entry : entries) {
        new_docs.push_back(entry.first);
        materialized.push_back(entry.second);
    }
    return std::unique_ptr<SortingNumericDocValues>(new SortingNumericDocValues(std::move(new_docs), std::move(materialized)));
}

int32_t SortingNumericDocValues::doc_id() const {
    return doc_;
}

Result<int32_t> SortingNumericDocValues::next_doc() {
    if (position_ < new_docs_.size()) {
        doc_ = new_docs_[position_];
        position_ += 1;
    } else {
        doc_ = NO_MORE_DOCS;
    }
    return doc_;
}

Result<int32_t> SortingNumericDocValues::advance(int32_t target) {
    auto begin = new_docs_.begin() + static_cast<std::ptrdiff_t>(position_);
    auto found = std::lower_bound(begin, new_docs_.end(), target);
    if (found == new_docs_.end()) {
        position_ = new_docs_.size();
        doc_ = NO_MORE_DOCS;
        return doc_;
    }
    position_ = static_cast<size_t>(found - new_docs_.begin()) + 1;
    doc_ = *found;
    return doc_;
}

Result<int64_t> SortingNumericDocValues::cost() const {
    return static_cast<int64_t>(new_docs_.size());
}

Result<bool> SortingNumericDocValues::advance_exact(int32_t target) {
    auto found = find_doc(new_docs_, target, position_);
    if (!found.has_value()) {
        return false;
    }
    position_ = found.value() + 1;
    doc_ = target;
    return true;
}

Result<int64_t> SortingNumericDocValues::long_value() {
    if (doc_ < 0 || position_ == 0) {
        return err(LuceneError::illegal_state("the iterator is not positioned on a document"));
    }
    return values_[position_ - 1];
}

// ---- SortingBinaryDocValues ----

SortingBinaryDocValues::SortingBinaryDocValues(std::vector<int32_t> new_docs, std::vector<BytesRef> values)
        : new_docs_(std::move(new_docs)), values_(std::move(values)) {}

Result<std::unique_ptr<SortingBinaryDocValues>> SortingBinaryDocValues::create(
        std::unique_ptr<BinaryDocValues> values, const IndexSortDocMap &doc_map) {
    if (values == nullptr) {
        return err(LuceneError::illegal_argument("the doc values must not be null"));
    }
    std::vector<std::pair<int32_t, BytesRef>> entries;
    auto collected = collect(*values, doc_map, [&values, &entries](int32_t new_doc, int32_t /*old_doc*/) {
        auto value = values->binary_value();
        if (!value.has_value()) {
            return VoidResult(std::unexpected(std::move(value.error())));
        }
        entries.emplace_back(new_doc, value.value());
        return VoidResult{};
    });
    if (!collected.has_value()) {
        return std::unexpected(std::move(collected.error()));
    }
    sort_by_new_doc(entries);
    std::vector<int32_t> new_docs;
    std::vector<BytesRef> materialized;
    for (auto &entry : entries) {
        new_docs.push_back(entry.first);
        materialized.push_back(std::move(entry.second));
    }
    return std::unique_ptr<SortingBinaryDocValues>(new SortingBinaryDocValues(std::move(new_docs), std::move(materialized)));
}

int32_t SortingBinaryDocValues::doc_id() const {
    return doc_;
}

Result<int32_t> SortingBinaryDocValues::next_doc() {
    if (position_ < new_docs_.size()) {
        doc_ = new_docs_[position_];
        position_ += 1;
    } else {
        doc_ = NO_MORE_DOCS;
    }
    return doc_;
}

Result<int32_t> SortingBinaryDocValues::advance(int32_t target) {
    auto begin = new_docs_.begin() + static_cast<std::ptrdiff_t>(position_);
    auto found = std::lower_bound(begin, new_docs_.end(), target);
    if (found == new_docs_.end()) {
        position_ = new_docs_.size();
        doc_ = NO_MORE_DOCS;
        return doc_;
    }
    position_ = static_cast<size_t>(found - new_docs_.begin()) + 1;
    doc_ = *found;
    return doc_;
}

Result<int64_t> SortingBinaryDocValues::cost() const {
    return static_cast<int64_t>(new_docs_.size());
}

Result<bool> SortingBinaryDocValues::advance_exact(int32_t target) {
    auto found = find_doc(new_docs_, target, position_);
    if (!found.has_value()) {
        return false;
    }
    position_ = found.value() + 1;
    doc_ = target;
    return true;
}

Result<BytesRef> SortingBinaryDocValues::binary_value() {
    if (doc_ < 0 || position_ == 0) {
        return err(LuceneError::illegal_state("the iterator is not positioned on a document"));
    }
    return values_[position_ - 1];
}

// ---- SortingSortedDocValues ----

SortingSortedDocValues::SortingSortedDocValues(std::vector<int32_t> new_docs, std::vector<int32_t> ords,
                                               std::unique_ptr<SortedDocValues> values)
        : new_docs_(std::move(new_docs)), ords_(std::move(ords)), values_(std::move(values)) {}

Result<std::unique_ptr<SortingSortedDocValues>> SortingSortedDocValues::create(
        std::unique_ptr<SortedDocValues> values, const IndexSortDocMap &doc_map) {
    if (values == nullptr) {
        return err(LuceneError::illegal_argument("the doc values must not be null"));
    }
    std::vector<std::pair<int32_t, int32_t>> entries;
    auto collected = collect(*values, doc_map, [&values, &entries](int32_t new_doc, int32_t /*old_doc*/) {
        auto ord = values->ord_value();
        if (!ord.has_value()) {
            return VoidResult(std::unexpected(std::move(ord.error())));
        }
        entries.emplace_back(new_doc, ord.value());
        return VoidResult{};
    });
    if (!collected.has_value()) {
        return std::unexpected(std::move(collected.error()));
    }
    sort_by_new_doc(entries);
    std::vector<int32_t> new_docs;
    std::vector<int32_t> ords;
    for (auto &entry : entries) {
        new_docs.push_back(entry.first);
        ords.push_back(entry.second);
    }
    return std::unique_ptr<SortingSortedDocValues>(
            new SortingSortedDocValues(std::move(new_docs), std::move(ords), std::move(values)));
}

int32_t SortingSortedDocValues::doc_id() const {
    return doc_;
}

Result<int32_t> SortingSortedDocValues::next_doc() {
    if (position_ < new_docs_.size()) {
        doc_ = new_docs_[position_];
        position_ += 1;
    } else {
        doc_ = NO_MORE_DOCS;
    }
    return doc_;
}

Result<int32_t> SortingSortedDocValues::advance(int32_t target) {
    auto begin = new_docs_.begin() + static_cast<std::ptrdiff_t>(position_);
    auto found = std::lower_bound(begin, new_docs_.end(), target);
    if (found == new_docs_.end()) {
        position_ = new_docs_.size();
        doc_ = NO_MORE_DOCS;
        return doc_;
    }
    position_ = static_cast<size_t>(found - new_docs_.begin()) + 1;
    doc_ = *found;
    return doc_;
}

Result<int64_t> SortingSortedDocValues::cost() const {
    return static_cast<int64_t>(new_docs_.size());
}

Result<bool> SortingSortedDocValues::advance_exact(int32_t target) {
    auto found = find_doc(new_docs_, target, position_);
    if (!found.has_value()) {
        return false;
    }
    position_ = found.value() + 1;
    doc_ = target;
    return true;
}

Result<int32_t> SortingSortedDocValues::ord_value() {
    if (doc_ < 0 || position_ == 0) {
        return err(LuceneError::illegal_state("the iterator is not positioned on a document"));
    }
    return ords_[position_ - 1];
}

Result<BytesRef> SortingSortedDocValues::lookup_ord(int32_t ord) {
    return values_->lookup_ord(ord);
}

Result<int32_t> SortingSortedDocValues::get_value_count() const {
    return values_->get_value_count();
}

// ---- SortingSortedNumericDocValues ----

SortingSortedNumericDocValues::SortingSortedNumericDocValues(std::vector<int32_t> new_docs,
                                                             std::vector<std::vector<int64_t>> values)
        : new_docs_(std::move(new_docs)), values_(std::move(values)) {}

Result<std::unique_ptr<SortingSortedNumericDocValues>> SortingSortedNumericDocValues::create(
        std::unique_ptr<SortedNumericDocValues> values, const IndexSortDocMap &doc_map) {
    if (values == nullptr) {
        return err(LuceneError::illegal_argument("the doc values must not be null"));
    }
    std::vector<std::pair<int32_t, std::vector<int64_t>>> entries;
    auto collected = collect(*values, doc_map, [&values, &entries](int32_t new_doc, int32_t /*old_doc*/) {
        auto count = values->doc_value_count();
        if (!count.has_value()) {
            return VoidResult(std::unexpected(std::move(count.error())));
        }
        std::vector<int64_t> doc_values;
        doc_values.reserve(static_cast<size_t>(count.value()));
        for (int32_t i = 0; i < count.value(); ++i) {
            auto value = values->next_value();
            if (!value.has_value()) {
                return VoidResult(std::unexpected(std::move(value.error())));
            }
            doc_values.push_back(value.value());
        }
        entries.emplace_back(new_doc, std::move(doc_values));
        return VoidResult{};
    });
    if (!collected.has_value()) {
        return std::unexpected(std::move(collected.error()));
    }
    sort_by_new_doc(entries);
    std::vector<int32_t> new_docs;
    std::vector<std::vector<int64_t>> materialized;
    for (auto &entry : entries) {
        new_docs.push_back(entry.first);
        materialized.push_back(std::move(entry.second));
    }
    return std::unique_ptr<SortingSortedNumericDocValues>(
            new SortingSortedNumericDocValues(std::move(new_docs), std::move(materialized)));
}

int32_t SortingSortedNumericDocValues::doc_id() const {
    return doc_;
}

Result<int32_t> SortingSortedNumericDocValues::next_doc() {
    if (position_ < new_docs_.size()) {
        doc_ = new_docs_[position_];
        position_ += 1;
        value_position_ = 0;
    } else {
        doc_ = NO_MORE_DOCS;
    }
    return doc_;
}

Result<int32_t> SortingSortedNumericDocValues::advance(int32_t target) {
    auto begin = new_docs_.begin() + static_cast<std::ptrdiff_t>(position_);
    auto found = std::lower_bound(begin, new_docs_.end(), target);
    if (found == new_docs_.end()) {
        position_ = new_docs_.size();
        doc_ = NO_MORE_DOCS;
        return doc_;
    }
    position_ = static_cast<size_t>(found - new_docs_.begin()) + 1;
    doc_ = *found;
    value_position_ = 0;
    return doc_;
}

Result<int64_t> SortingSortedNumericDocValues::cost() const {
    return static_cast<int64_t>(new_docs_.size());
}

Result<bool> SortingSortedNumericDocValues::advance_exact(int32_t target) {
    auto found = find_doc(new_docs_, target, position_);
    if (!found.has_value()) {
        return false;
    }
    position_ = found.value() + 1;
    doc_ = target;
    value_position_ = 0;
    return true;
}

Result<int64_t> SortingSortedNumericDocValues::next_value() {
    if (doc_ < 0 || position_ == 0) {
        return err(LuceneError::illegal_state("the iterator is not positioned on a document"));
    }
    const std::vector<int64_t> &doc_values = values_[position_ - 1];
    if (value_position_ >= doc_values.size()) {
        return err(LuceneError::illegal_state("no more values for this document"));
    }
    return doc_values[value_position_++];
}

Result<int32_t> SortingSortedNumericDocValues::doc_value_count() {
    if (doc_ < 0 || position_ == 0) {
        return err(LuceneError::illegal_state("the iterator is not positioned on a document"));
    }
    return static_cast<int32_t>(values_[position_ - 1].size());
}

// ---- SortingSortedSetDocValues ----

SortingSortedSetDocValues::SortingSortedSetDocValues(std::vector<int32_t> new_docs, std::vector<std::vector<int64_t>> ords,
                                                     std::unique_ptr<SortedSetDocValues> values)
        : new_docs_(std::move(new_docs)), ords_(std::move(ords)), values_(std::move(values)) {}

Result<std::unique_ptr<SortingSortedSetDocValues>> SortingSortedSetDocValues::create(
        std::unique_ptr<SortedSetDocValues> values, const IndexSortDocMap &doc_map) {
    if (values == nullptr) {
        return err(LuceneError::illegal_argument("the doc values must not be null"));
    }
    std::vector<std::pair<int32_t, std::vector<int64_t>>> entries;
    auto collected = collect(*values, doc_map, [&values, &entries](int32_t new_doc, int32_t /*old_doc*/) {
        auto count = values->doc_value_count();
        if (!count.has_value()) {
            return VoidResult(std::unexpected(std::move(count.error())));
        }
        std::vector<int64_t> doc_ords;
        doc_ords.reserve(static_cast<size_t>(count.value()));
        for (int32_t i = 0; i < count.value(); ++i) {
            auto ord = values->next_ord();
            if (!ord.has_value()) {
                return VoidResult(std::unexpected(std::move(ord.error())));
            }
            doc_ords.push_back(ord.value());
        }
        entries.emplace_back(new_doc, std::move(doc_ords));
        return VoidResult{};
    });
    if (!collected.has_value()) {
        return std::unexpected(std::move(collected.error()));
    }
    sort_by_new_doc(entries);
    std::vector<int32_t> new_docs;
    std::vector<std::vector<int64_t>> materialized;
    for (auto &entry : entries) {
        new_docs.push_back(entry.first);
        materialized.push_back(std::move(entry.second));
    }
    return std::unique_ptr<SortingSortedSetDocValues>(
            new SortingSortedSetDocValues(std::move(new_docs), std::move(materialized), std::move(values)));
}

int32_t SortingSortedSetDocValues::doc_id() const {
    return doc_;
}

Result<int32_t> SortingSortedSetDocValues::next_doc() {
    if (position_ < new_docs_.size()) {
        doc_ = new_docs_[position_];
        position_ += 1;
        ord_position_ = 0;
    } else {
        doc_ = NO_MORE_DOCS;
    }
    return doc_;
}

Result<int32_t> SortingSortedSetDocValues::advance(int32_t target) {
    auto begin = new_docs_.begin() + static_cast<std::ptrdiff_t>(position_);
    auto found = std::lower_bound(begin, new_docs_.end(), target);
    if (found == new_docs_.end()) {
        position_ = new_docs_.size();
        doc_ = NO_MORE_DOCS;
        return doc_;
    }
    position_ = static_cast<size_t>(found - new_docs_.begin()) + 1;
    doc_ = *found;
    ord_position_ = 0;
    return doc_;
}

Result<int64_t> SortingSortedSetDocValues::cost() const {
    return static_cast<int64_t>(new_docs_.size());
}

Result<bool> SortingSortedSetDocValues::advance_exact(int32_t target) {
    auto found = find_doc(new_docs_, target, position_);
    if (!found.has_value()) {
        return false;
    }
    position_ = found.value() + 1;
    doc_ = target;
    ord_position_ = 0;
    return true;
}

Result<int64_t> SortingSortedSetDocValues::next_ord() {
    if (doc_ < 0 || position_ == 0) {
        return err(LuceneError::illegal_state("the iterator is not positioned on a document"));
    }
    const std::vector<int64_t> &doc_ords = ords_[position_ - 1];
    if (ord_position_ >= doc_ords.size()) {
        return NO_MORE_ORDS;
    }
    return doc_ords[ord_position_++];
}

Result<int32_t> SortingSortedSetDocValues::doc_value_count() {
    if (doc_ < 0 || position_ == 0) {
        return err(LuceneError::illegal_state("the iterator is not positioned on a document"));
    }
    return static_cast<int32_t>(ords_[position_ - 1].size());
}

Result<BytesRef> SortingSortedSetDocValues::lookup_ord(int64_t ord) {
    return values_->lookup_ord(ord);
}

Result<int64_t> SortingSortedSetDocValues::get_value_count() const {
    return values_->get_value_count();
}

} // namespace lucene
