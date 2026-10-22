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

module lucene_core:base_composite_reader.impl;

import :base_composite_reader;

import :document;
import :error;
import :fields;
import :index_reader;
import :reader_util;
import :stored_field_visitor;
import :stored_fields;
import :term;
import :term_vectors;
import :types;

import std;

namespace lucene {

namespace {

/// `IndexWriter::MAX_DOCS` / `SegmentInfos::MAX_DOCS`: the largest document
/// count an index may hold, kept slightly below `INT32_MAX` so that
/// `max_doc + 1` cannot overflow.
constexpr int32_t kMaxDocs = std::numeric_limits<int32_t>::max() - 128;

} // namespace

Result<int32_t> reader_index(int32_t doc_id, int32_t max_doc, std::span<const int32_t> starts) {
    if (doc_id < 0 || doc_id >= max_doc) {
        return err(LuceneError::illegal_argument("docID must be >= 0 and < maxDoc=" + std::to_string(max_doc) +
                                                 " (got docID=" + std::to_string(doc_id) + ")"));
    }
    const int32_t index = ReaderUtil::sub_index(doc_id, starts);
    if (index < 0) {
        return err(LuceneError::illegal_state("index should be >= 0"));
    }
    return index;
}

// ---------------------------------------------------------------------------
// MultiStoredFields
// ---------------------------------------------------------------------------

MultiStoredFields::MultiStoredFields(std::vector<std::shared_ptr<IndexReader>> sub_readers,
                                     std::vector<int32_t> starts, int32_t max_doc)
        : sub_readers_(std::move(sub_readers)),
          starts_(std::move(starts)),
          cursors_(sub_readers_.size()),
          max_doc_(max_doc) {}

Result<StoredFields *> MultiStoredFields::cursor_for(int32_t doc_id, int32_t &local_doc) {
    auto index = reader_index(doc_id, max_doc_, starts_);
    if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
    }
    const auto slot = static_cast<size_t>(index.value());
    if (cursors_[slot] == nullptr) {
        auto opened = sub_readers_[slot]->stored_fields();
        if (!opened.has_value()) {
            return std::unexpected(std::move(opened.error()));
        }
        if (opened.value() == nullptr) {
            return err(LuceneError::illegal_state("sub-reader has no stored fields reader"));
        }
        cursors_[slot] = std::move(opened.value());
    }
    local_doc = doc_id - starts_[slot];
    return cursors_[slot].get();
}

VoidResult MultiStoredFields::prefetch(int32_t doc_id) {
    int32_t local_doc = 0;
    auto cursor = cursor_for(doc_id, local_doc);
    if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
    }
    return cursor.value()->prefetch(local_doc);
}

VoidResult MultiStoredFields::document_with_visitor(int32_t doc_id, StoredFieldVisitor &visitor) {
    int32_t local_doc = 0;
    auto cursor = cursor_for(doc_id, local_doc);
    if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
    }
    return cursor.value()->document_with_visitor(local_doc, visitor);
}

Result<Document> MultiStoredFields::document(int32_t doc_id) {
    int32_t local_doc = 0;
    auto cursor = cursor_for(doc_id, local_doc);
    if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
    }
    return cursor.value()->document(local_doc);
}

// ---------------------------------------------------------------------------
// MultiTermVectors
// ---------------------------------------------------------------------------

MultiTermVectors::MultiTermVectors(std::vector<std::shared_ptr<IndexReader>> sub_readers,
                                   std::vector<int32_t> starts, int32_t max_doc)
        : sub_readers_(std::move(sub_readers)),
          starts_(std::move(starts)),
          cursors_(sub_readers_.size()),
          max_doc_(max_doc) {}

Result<TermVectors *> MultiTermVectors::cursor_for(int32_t doc_id, int32_t &local_doc) {
    auto index = reader_index(doc_id, max_doc_, starts_);
    if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
    }
    const auto slot = static_cast<size_t>(index.value());
    if (cursors_[slot] == nullptr) {
        auto opened = sub_readers_[slot]->term_vectors();
        if (!opened.has_value()) {
            return std::unexpected(std::move(opened.error()));
        }
        if (opened.value() == nullptr) {
            return err(LuceneError::illegal_state("sub-reader has no term vectors reader"));
        }
        cursors_[slot] = std::move(opened.value());
    }
    local_doc = doc_id - starts_[slot];
    return cursors_[slot].get();
}

VoidResult MultiTermVectors::prefetch(int32_t doc_id) {
    int32_t local_doc = 0;
    auto cursor = cursor_for(doc_id, local_doc);
    if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
    }
    return cursor.value()->prefetch(local_doc);
}

Result<std::unique_ptr<Fields>> MultiTermVectors::get(int32_t doc) {
    int32_t local_doc = 0;
    auto cursor = cursor_for(doc, local_doc);
    if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
    }
    return cursor.value()->get(local_doc);
}

// ---------------------------------------------------------------------------
// CompositeReaderBase
// ---------------------------------------------------------------------------

Result<CompositeReaderBase> CompositeReaderBase::create(std::vector<std::shared_ptr<IndexReader>> sub_readers) {
    CompositeReaderBase base;
    base.sub_readers_ = std::move(sub_readers);
    base.starts_.assign(base.sub_readers_.size() + 1, 0);

    int64_t max_doc = 0;
    for (size_t i = 0; i < base.sub_readers_.size(); ++i) {
        const auto &reader = base.sub_readers_[i];
        if (reader == nullptr) {
            return err(LuceneError::illegal_argument("sub-reader must not be null"));
        }
        auto sub_max_doc = reader->max_doc();
        if (!sub_max_doc.has_value()) {
            return std::unexpected(std::move(sub_max_doc.error()));
        }
        base.starts_[i] = static_cast<int32_t>(max_doc);
        max_doc += sub_max_doc.value();
    }
    if (max_doc > kMaxDocs) {
        return err(LuceneError::illegal_argument("Too many documents: composite IndexReaders cannot exceed " +
                                                 std::to_string(kMaxDocs) + ", total maxDoc=" +
                                                 std::to_string(max_doc)));
    }
    base.max_doc_ = static_cast<int32_t>(max_doc);
    base.starts_[base.sub_readers_.size()] = base.max_doc_;
    return base;
}

Result<int32_t> CompositeReaderBase::num_docs() const {
    int32_t total = 0;
    for (const auto &reader : sub_readers_) {
        auto sub = reader->num_docs();
        if (!sub.has_value()) {
            return std::unexpected(std::move(sub.error()));
        }
        total += sub.value();
    }
    return total;
}

Result<int32_t> CompositeReaderBase::reader_base(size_t index) const {
    if (index >= sub_readers_.size()) {
        return err(LuceneError::illegal_argument("reader_index must be < sub_readers().size()"));
    }
    return starts_[index];
}

Result<int32_t> CompositeReaderBase::doc_freq(const Term &term) const {
    int32_t total = 0;
    for (const auto &reader : sub_readers_) {
        auto sub = reader->doc_freq(term);
        if (!sub.has_value()) {
            return std::unexpected(std::move(sub.error()));
        }
        total += sub.value();
    }
    return total;
}

Result<int64_t> CompositeReaderBase::total_term_freq(const Term &term) const {
    int64_t total = 0;
    for (const auto &reader : sub_readers_) {
        auto sub = reader->total_term_freq(term);
        if (!sub.has_value()) {
            return std::unexpected(std::move(sub.error()));
        }
        total += sub.value();
    }
    return total;
}

Result<int64_t> CompositeReaderBase::sum_doc_freq(std::string_view field) const {
    int64_t total = 0;
    for (const auto &reader : sub_readers_) {
        auto sub = reader->sum_doc_freq(field);
        if (!sub.has_value()) {
            return std::unexpected(std::move(sub.error()));
        }
        // `-1` means "unknown"; a composite cannot know it either.
        if (sub.value() < 0) {
            return -1;
        }
        total += sub.value();
    }
    return total;
}

Result<int32_t> CompositeReaderBase::doc_count(std::string_view field) const {
    int32_t total = 0;
    for (const auto &reader : sub_readers_) {
        auto sub = reader->doc_count(field);
        if (!sub.has_value()) {
            return std::unexpected(std::move(sub.error()));
        }
        if (sub.value() < 0) {
            return -1;
        }
        total += sub.value();
    }
    return total;
}

Result<int64_t> CompositeReaderBase::sum_total_term_freq(std::string_view field) const {
    int64_t total = 0;
    for (const auto &reader : sub_readers_) {
        auto sub = reader->sum_total_term_freq(field);
        if (!sub.has_value()) {
            return std::unexpected(std::move(sub.error()));
        }
        if (sub.value() < 0) {
            return -1;
        }
        total += sub.value();
    }
    return total;
}

Result<std::unique_ptr<StoredFields>> CompositeReaderBase::stored_fields() const {
    return std::unique_ptr<StoredFields>(new MultiStoredFields(sub_readers_, starts_, max_doc_));
}

Result<std::unique_ptr<TermVectors>> CompositeReaderBase::term_vectors() const {
    return std::unique_ptr<TermVectors>(new MultiTermVectors(sub_readers_, starts_, max_doc_));
}

} // namespace lucene
