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

module lucene_core:doc_values_merge.impl;

import :doc_values_merge;

import std;

import :bytes_ref;
import :doc_id_merger;
import :error;
import :numeric_doc_values;

namespace lucene {

namespace {

/// Wraps `subs` into the `DocIdMergerSub` shells `DocIdMerger` expects.
template <class Sub>
[[nodiscard]] std::vector<DocIdMergerSub<Sub>> wrap_subs(std::vector<Sub> subs) {
    std::vector<DocIdMergerSub<Sub>> wrapped;
    wrapped.reserve(subs.size());
    for (auto &sub : subs) {
        wrapped.emplace_back(std::move(sub));
    }
    return wrapped;
}

[[nodiscard]] Result<int32_t> check_doc_index(size_t size, int32_t doc_id) {
    if (doc_id < 0 || static_cast<size_t>(doc_id) >= size) {
        return err(LuceneError::array_index_out_of_bounds(
                std::format("doc id {} is out of bounds for {} documents", doc_id, size)));
    }
    return doc_id;
}

/// Binary search in a sorted, deduplicated term dictionary.
[[nodiscard]] int32_t dictionary_lookup(const std::vector<BytesRef> &terms, std::span<const uint8_t> key) {
    size_t low = 0;
    size_t high = terms.size();
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        if (terms[mid].compare_to(key) < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low < terms.size() && terms[low].bytes_equals(key)) {
        return static_cast<int32_t>(low);
    }
    return -static_cast<int32_t>(low) - 1;
}

} // namespace

// ---------------------------------------------------------------------------
// numeric
// ---------------------------------------------------------------------------

NumericDocValuesSub::NumericDocValuesSub(std::unique_ptr<NumericDocValues> values, const DocMap &doc_map)
        : values_(std::move(values)), doc_map_(&doc_map) {}

Result<int32_t> NumericDocValuesSub::next_doc() {
    return values_->next_doc();
}

NumericDocValuesMerge::NumericDocValuesMerge(DocIdMerger<NumericDocValuesSub> merger)
        : merger_(std::move(merger)) {}

Result<std::unique_ptr<NumericDocValuesMerge>> NumericDocValuesMerge::create(std::vector<NumericDocValuesSub> subs,
                                                                            bool needs_index_sort) {
    auto merger = DocIdMerger<NumericDocValuesSub>::of(wrap_subs(std::move(subs)), needs_index_sort);
    if (!merger.has_value()) {
        return std::unexpected(std::move(merger.error()));
    }
    return std::unique_ptr<NumericDocValuesMerge>(new NumericDocValuesMerge(std::move(merger.value())));
}

Result<int32_t> NumericDocValuesMerge::next_doc() {
    auto next = merger_.next();
    if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
    }
    current_ = next.value();
    if (!current_.has_value()) {
        doc_id_ = NO_MORE_DOCS;
        return NO_MORE_DOCS;
    }
    doc_id_ = merger_.subs()[current_.value()].mapped_doc_id;
    return doc_id_;
}

Result<int32_t> NumericDocValuesMerge::advance(int32_t target) {
    return err(LuceneError::unsupported_operation(
            std::format("advance({}) is not supported while merging doc values", target)));
}

Result<bool> NumericDocValuesMerge::advance_exact(int32_t target) {
    return err(LuceneError::unsupported_operation(
            std::format("advance_exact({}) is not supported while merging doc values", target)));
}

Result<int64_t> NumericDocValuesMerge::long_value() {
    if (!current_.has_value()) {
        return err(LuceneError::unreachable("long_value called before next_doc"));
    }
    return merger_.subs()[current_.value()].sub.values().long_value();
}

// ---------------------------------------------------------------------------
// binary
// ---------------------------------------------------------------------------

BinaryDocValuesSub::BinaryDocValuesSub(std::unique_ptr<BinaryDocValues> values, const DocMap &doc_map)
        : values_(std::move(values)), doc_map_(&doc_map) {}

Result<int32_t> BinaryDocValuesSub::next_doc() {
    return values_->next_doc();
}

BinaryDocValuesMerge::BinaryDocValuesMerge(DocIdMerger<BinaryDocValuesSub> merger) : merger_(std::move(merger)) {}

Result<std::unique_ptr<BinaryDocValuesMerge>> BinaryDocValuesMerge::create(std::vector<BinaryDocValuesSub> subs,
                                                                          bool needs_index_sort) {
    auto merger = DocIdMerger<BinaryDocValuesSub>::of(wrap_subs(std::move(subs)), needs_index_sort);
    if (!merger.has_value()) {
        return std::unexpected(std::move(merger.error()));
    }
    return std::unique_ptr<BinaryDocValuesMerge>(new BinaryDocValuesMerge(std::move(merger.value())));
}

Result<int32_t> BinaryDocValuesMerge::next_doc() {
    auto next = merger_.next();
    if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
    }
    current_ = next.value();
    if (!current_.has_value()) {
        doc_id_ = NO_MORE_DOCS;
        return NO_MORE_DOCS;
    }
    doc_id_ = merger_.subs()[current_.value()].mapped_doc_id;
    return doc_id_;
}

Result<int32_t> BinaryDocValuesMerge::advance(int32_t target) {
    return err(LuceneError::unsupported_operation(
            std::format("advance({}) is not supported while merging doc values", target)));
}

Result<bool> BinaryDocValuesMerge::advance_exact(int32_t target) {
    return err(LuceneError::unsupported_operation(
            std::format("advance_exact({}) is not supported while merging doc values", target)));
}

Result<BytesRef> BinaryDocValuesMerge::binary_value() {
    if (!current_.has_value()) {
        return err(LuceneError::unreachable("binary_value called before next_doc"));
    }
    return merger_.subs()[current_.value()].sub.values().binary_value();
}

// ---------------------------------------------------------------------------
// in-memory numeric
// ---------------------------------------------------------------------------

InMemoryNumericDocValues::InMemoryNumericDocValues(std::vector<int64_t> values) : values_(std::move(values)) {}

Result<int32_t> InMemoryNumericDocValues::next_doc() {
    if (doc_id_ + 1 >= static_cast<int32_t>(values_.size())) {
        doc_id_ = NO_MORE_DOCS;
        return NO_MORE_DOCS;
    }
    ++doc_id_;
    return doc_id_;
}

Result<int32_t> InMemoryNumericDocValues::advance(int32_t target) {
    if (target <= doc_id_) {
        return err(LuceneError::illegal_argument(std::format("advance({}) must move forward from {}", target, doc_id_)));
    }
    if (target >= static_cast<int32_t>(values_.size())) {
        doc_id_ = NO_MORE_DOCS;
        return NO_MORE_DOCS;
    }
    doc_id_ = target;
    return doc_id_;
}

Result<bool> InMemoryNumericDocValues::advance_exact(int32_t target) {
    if (target >= static_cast<int32_t>(values_.size())) {
        doc_id_ = NO_MORE_DOCS;
        return false;
    }
    doc_id_ = target;
    return true;
}

Result<int64_t> InMemoryNumericDocValues::long_value() {
    if (auto checked = check_doc_index(values_.size(), doc_id_); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return values_[static_cast<size_t>(doc_id_)];
}

// ---------------------------------------------------------------------------
// in-memory binary
// ---------------------------------------------------------------------------

InMemoryBinaryDocValues::InMemoryBinaryDocValues(std::vector<BytesRef> values) : values_(std::move(values)) {}

Result<int32_t> InMemoryBinaryDocValues::next_doc() {
    if (doc_id_ + 1 >= static_cast<int32_t>(values_.size())) {
        doc_id_ = NO_MORE_DOCS;
        return NO_MORE_DOCS;
    }
    ++doc_id_;
    return doc_id_;
}

Result<int32_t> InMemoryBinaryDocValues::advance(int32_t target) {
    if (target <= doc_id_) {
        return err(LuceneError::illegal_argument(std::format("advance({}) must move forward from {}", target, doc_id_)));
    }
    if (target >= static_cast<int32_t>(values_.size())) {
        doc_id_ = NO_MORE_DOCS;
        return NO_MORE_DOCS;
    }
    doc_id_ = target;
    return doc_id_;
}

Result<bool> InMemoryBinaryDocValues::advance_exact(int32_t target) {
    if (target >= static_cast<int32_t>(values_.size())) {
        doc_id_ = NO_MORE_DOCS;
        return false;
    }
    doc_id_ = target;
    return true;
}

Result<BytesRef> InMemoryBinaryDocValues::binary_value() {
    if (auto checked = check_doc_index(values_.size(), doc_id_); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return values_[static_cast<size_t>(doc_id_)];
}

// ---------------------------------------------------------------------------
// in-memory sorted
// ---------------------------------------------------------------------------

InMemorySortedDocValues::InMemorySortedDocValues(std::vector<int32_t> ords, std::vector<BytesRef> terms)
        : ords_(std::move(ords)), terms_(std::move(terms)) {}

Result<int32_t> InMemorySortedDocValues::next_doc() {
    while (static_cast<size_t>(next_index_) < ords_.size()) {
        const int32_t doc = next_index_++;
        if (ords_[static_cast<size_t>(doc)] >= 0) {
            doc_id_ = doc;
            return doc;
        }
    }
    doc_id_ = NO_MORE_DOCS;
    return NO_MORE_DOCS;
}

Result<int32_t> InMemorySortedDocValues::advance(int32_t target) {
    next_index_ = std::max(next_index_, target);
    return next_doc();
}

Result<bool> InMemorySortedDocValues::advance_exact(int32_t target) {
    int32_t index = std::max(next_index_, target);
    while (static_cast<size_t>(index) < ords_.size() && ords_[static_cast<size_t>(index)] < 0) {
        ++index;
    }
    doc_id_ = target;
    if (static_cast<size_t>(index) >= ords_.size()) {
        next_index_ = index;
        return false;
    }
    if (index == target) {
        next_index_ = index + 1;
        return true;
    }
    next_index_ = index;
    return false;
}

Result<int32_t> InMemorySortedDocValues::ord_value() {
    if (auto checked = check_doc_index(ords_.size(), doc_id_); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return ords_[static_cast<size_t>(doc_id_)];
}

Result<BytesRef> InMemorySortedDocValues::lookup_ord(int32_t ord) {
    if (ord < 0 || static_cast<size_t>(ord) >= terms_.size()) {
        return err(LuceneError::array_index_out_of_bounds(std::format("ord {} out of range", ord)));
    }
    return terms_[static_cast<size_t>(ord)];
}

Result<int32_t> InMemorySortedDocValues::get_value_count() const {
    return static_cast<int32_t>(terms_.size());
}

Result<int32_t> InMemorySortedDocValues::lookup_term(std::span<const uint8_t> key) {
    return dictionary_lookup(terms_, key);
}

// ---------------------------------------------------------------------------
// in-memory sorted set
// ---------------------------------------------------------------------------

InMemorySortedSetDocValues::InMemorySortedSetDocValues(std::vector<std::vector<int64_t>> ords,
                                                       std::vector<BytesRef> terms)
        : ords_(std::move(ords)), terms_(std::move(terms)) {}

Result<int32_t> InMemorySortedSetDocValues::next_doc() {
    while (static_cast<size_t>(next_index_) < ords_.size()) {
        const int32_t doc = next_index_++;
        if (!ords_[static_cast<size_t>(doc)].empty()) {
            doc_id_ = doc;
            ord_index_ = 0;
            return doc;
        }
    }
    doc_id_ = NO_MORE_DOCS;
    return NO_MORE_DOCS;
}

Result<int32_t> InMemorySortedSetDocValues::advance(int32_t target) {
    next_index_ = std::max(next_index_, target);
    return next_doc();
}

Result<bool> InMemorySortedSetDocValues::advance_exact(int32_t target) {
    int32_t index = std::max(next_index_, target);
    while (static_cast<size_t>(index) < ords_.size() && ords_[static_cast<size_t>(index)].empty()) {
        ++index;
    }
    doc_id_ = target;
    if (static_cast<size_t>(index) >= ords_.size()) {
        next_index_ = index;
        return false;
    }
    if (index == target) {
        next_index_ = index + 1;
        ord_index_ = 0;
        return true;
    }
    next_index_ = index;
    return false;
}

Result<int64_t> InMemorySortedSetDocValues::next_ord() {
    if (auto checked = check_doc_index(ords_.size(), doc_id_); !checked.has_value()) {
        return NO_MORE_ORDS;
    }
    const auto &ords = ords_[static_cast<size_t>(doc_id_)];
    if (ord_index_ >= ords.size()) {
        return NO_MORE_ORDS;
    }
    return ords[ord_index_++];
}

Result<int32_t> InMemorySortedSetDocValues::doc_value_count() {
    if (auto checked = check_doc_index(ords_.size(), doc_id_); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return static_cast<int32_t>(ords_[static_cast<size_t>(doc_id_)].size());
}

Result<BytesRef> InMemorySortedSetDocValues::lookup_ord(int64_t ord) {
    if (ord < 0 || static_cast<size_t>(ord) >= terms_.size()) {
        return err(LuceneError::array_index_out_of_bounds(std::format("ord {} out of range", ord)));
    }
    return terms_[static_cast<size_t>(ord)];
}

Result<int64_t> InMemorySortedSetDocValues::get_value_count() const {
    return static_cast<int64_t>(terms_.size());
}

Result<int64_t> InMemorySortedSetDocValues::lookup_term(std::span<const uint8_t> key) {
    return static_cast<int64_t>(dictionary_lookup(terms_, key));
}

// ---------------------------------------------------------------------------
// in-memory sorted numeric
// ---------------------------------------------------------------------------

InMemorySortedNumericDocValues::InMemorySortedNumericDocValues(std::vector<std::vector<int64_t>> values)
        : values_(std::move(values)) {
    single_valued_ = std::ranges::all_of(values_, [](const auto &value) { return value.size() == 1; });
}

Result<int32_t> InMemorySortedNumericDocValues::next_doc() {
    while (static_cast<size_t>(next_index_) < values_.size()) {
        const int32_t doc = next_index_++;
        if (!values_[static_cast<size_t>(doc)].empty()) {
            doc_id_ = doc;
            value_index_ = 0;
            return doc;
        }
    }
    doc_id_ = NO_MORE_DOCS;
    return NO_MORE_DOCS;
}

Result<int32_t> InMemorySortedNumericDocValues::advance(int32_t target) {
    next_index_ = std::max(next_index_, target);
    return next_doc();
}

Result<bool> InMemorySortedNumericDocValues::advance_exact(int32_t target) {
    int32_t index = std::max(next_index_, target);
    while (static_cast<size_t>(index) < values_.size() && values_[static_cast<size_t>(index)].empty()) {
        ++index;
    }
    doc_id_ = target;
    if (static_cast<size_t>(index) >= values_.size()) {
        next_index_ = index;
        return false;
    }
    if (index == target) {
        next_index_ = index + 1;
        value_index_ = 0;
        return true;
    }
    next_index_ = index;
    return false;
}

Result<int64_t> InMemorySortedNumericDocValues::next_value() {
    if (auto checked = check_doc_index(values_.size(), doc_id_); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    const auto &values = values_[static_cast<size_t>(doc_id_)];
    if (value_index_ >= values.size()) {
        return err(LuceneError::illegal_state("no more values for this document"));
    }
    return values[value_index_++];
}

Result<int32_t> InMemorySortedNumericDocValues::doc_value_count() {
    if (auto checked = check_doc_index(values_.size(), doc_id_); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return static_cast<int32_t>(values_[static_cast<size_t>(doc_id_)].size());
}

Result<std::unique_ptr<NumericDocValues>> InMemorySortedNumericDocValues::get_numeric_doc_values() {
    if (!single_valued_) {
        return err(LuceneError::unsupported_operation("this SortedNumericDocValues is not single-valued"));
    }
    std::vector<int64_t> flat;
    flat.reserve(values_.size());
    for (const auto &values : values_) {
        flat.push_back(values.empty() ? 0 : values[0]);
    }
    return std::unique_ptr<NumericDocValues>(new InMemoryNumericDocValues(std::move(flat)));
}

} // namespace lucene
