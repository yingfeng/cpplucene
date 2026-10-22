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

module lucene_core:sorted_numeric_doc_values_writer.impl;

import :sorted_numeric_doc_values_writer;

import :debug;
import :doc_id_set_iterator;
import :doc_values_consumer;
import :doc_values_producer;
import :doc_values_writer;
import :docs_with_field_set;
import :error;
import :field_infos;
import :numeric_doc_values;
import :numeric_doc_values_writer;
import :packed_long_values;
import :sorted_numeric_doc_values;
import :types;

import std;

namespace lucene {

namespace {

/// Number of bits per packed value: the widest `int64_t`, like the reference implementation's
/// `delta_packed_long_values_builder_default(COMPACT)`.
constexpr int32_t BITS_PER_VALUE = static_cast<int32_t>(sizeof(int64_t) * 8);

[[nodiscard]] PackedLongValues::Builder delta_builder() {
    return PackedLongValues::Builder(PackedLongValues::DEFAULT_PAGE_SIZE, BITS_PER_VALUE, 0, true);
}

} // namespace

// ---------------------------------------------------------------------------
// BufferedSortedNumericDocValues
// ---------------------------------------------------------------------------

BufferedSortedNumericDocValues::BufferedSortedNumericDocValues(
        std::shared_ptr<const PackedLongValues> values, std::shared_ptr<const PackedLongValues> value_counts,
        std::unique_ptr<DocIdSetIterator> docs_with_field)
        : values_(std::move(values)),
          value_counts_(std::move(value_counts)),
          values_iterator_(values_->iterator()),
          counts_iterator_(value_counts_->iterator()),
          docs_with_field_(std::move(docs_with_field)) {}

VoidResult BufferedSortedNumericDocValues::enter_doc() {
    auto count = counts_iterator_->next();
    if (!count.has_value()) {
        return std::unexpected(std::move(count.error()));
    }
    count_ = static_cast<int32_t>(count.value());
    upto_ = 0;
    return {};
}

Result<int32_t> BufferedSortedNumericDocValues::next_doc() {
    auto doc = docs_with_field_->next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        if (auto entered = enter_doc(); !entered.has_value()) {
            return std::unexpected(std::move(entered.error()));
        }
    }
    return doc.value();
}

Result<int32_t> BufferedSortedNumericDocValues::advance(int32_t target) {
    auto doc = docs_with_field_->advance(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        if (auto entered = enter_doc(); !entered.has_value()) {
            return std::unexpected(std::move(entered.error()));
        }
    }
    return doc.value();
}

Result<int64_t> BufferedSortedNumericDocValues::cost() const { return docs_with_field_->cost(); }

Result<bool> BufferedSortedNumericDocValues::advance_exact(int32_t target) {
    auto doc = docs_with_field_->advance(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != target) {
        return false;
    }
    if (auto entered = enter_doc(); !entered.has_value()) {
        return std::unexpected(std::move(entered.error()));
    }
    return true;
}

Result<int64_t> BufferedSortedNumericDocValues::next_value() {
    if (upto_ >= count_) {
        return err(LuceneError::illegal_state("next_value called more than doc_value_count times"));
    }
    upto_ += 1;
    return values_iterator_->next();
}

// ---------------------------------------------------------------------------
// SortedNumericDocValuesProducer
// ---------------------------------------------------------------------------

SortedNumericDocValuesProducer::SortedNumericDocValuesProducer(
        std::shared_ptr<FieldInfo> field_info, std::shared_ptr<const PackedLongValues> values,
        std::shared_ptr<const PackedLongValues> value_counts, const DocsWithFieldSet &docs_with_field)
        : field_info_(std::move(field_info)),
          values_(std::move(values)),
          value_counts_(std::move(value_counts)),
          docs_with_field_(&docs_with_field) {}

Result<std::unique_ptr<SortedNumericDocValues>> SortedNumericDocValuesProducer::get_sorted_numeric(
        const FieldInfo &field) const {
    if (field.name != field_info_->name) {
        return err(LuceneError::illegal_argument("wrong fieldInfo: expected \"" + field_info_->name + "\", got \"" +
                                                field.name + "\""));
    }
    auto docs = docs_with_field_->iterator();
    if (!docs.has_value()) {
        return std::unexpected(std::move(docs.error()));
    }

    if (value_counts_ == nullptr) {
        // Every document has exactly one value: emit the single-valued shape.
        auto numeric = std::make_unique<BufferedNumericDocValues>(values_, std::move(docs.value()));
        return std::unique_ptr<SortedNumericDocValues>(
                std::make_unique<SingletonSortedNumericDocValues>(std::move(numeric)));
    }
    return std::unique_ptr<SortedNumericDocValues>(std::make_unique<BufferedSortedNumericDocValues>(
            values_, value_counts_, std::move(docs.value())));
}

// ---------------------------------------------------------------------------
// SortedNumericDocValuesWriter
// ---------------------------------------------------------------------------

SortedNumericDocValuesWriter::SortedNumericDocValuesWriter(std::shared_ptr<FieldInfo> field_info)
        : field_info_(std::move(field_info)), pending_(delta_builder()) {}

VoidResult SortedNumericDocValuesWriter::add_value(int32_t doc_id, int64_t value) {
    debug_check(doc_id >= current_doc_, "the document ids must not go backwards");
    if (doc_id != current_doc_) {
        if (auto finished = finish_current_doc(); !finished.has_value()) {
            return finished;
        }
        current_doc_ = doc_id;
    }
    current_values_.push_back(value);
    current_upto_ += 1;
    return {};
}

VoidResult SortedNumericDocValuesWriter::finish_current_doc() {
    if (current_doc_ == -1) {
        return {};
    }
    if (current_upto_ > 1) {
        std::sort(current_values_.begin(), current_values_.begin() + static_cast<std::ptrdiff_t>(current_upto_));
    }
    for (size_t i = 0; i < current_upto_; ++i) {
        if (auto added = pending_.add(current_values_[i]); !added.has_value()) {
            return added;
        }
    }

    if (pending_counts_.has_value()) {
        if (auto added = pending_counts_->add(static_cast<int64_t>(current_upto_)); !added.has_value()) {
            return added;
        }
    } else if (current_upto_ != 1) {
        // The docs seen so far all had one value: materialize the count stream.
        auto counts = delta_builder();
        for (int32_t i = 0; i < docs_with_field_.cardinality(); ++i) {
            if (auto added = counts.add(1); !added.has_value()) {
                return added;
            }
        }
        if (auto added = counts.add(static_cast<int64_t>(current_upto_)); !added.has_value()) {
            return added;
        }
        pending_counts_.emplace(std::move(counts));
    }

    current_upto_ = 0;
    current_values_.clear();
    return docs_with_field_.add(current_doc_);
}

VoidResult SortedNumericDocValuesWriter::finish() {
    if (final_values_ != nullptr) {
        return {};
    }
    if (auto finished = finish_current_doc(); !finished.has_value()) {
        return finished;
    }

    auto values = pending_.build();
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    final_values_ = std::make_shared<const PackedLongValues>(std::move(values.value()));

    if (pending_counts_.has_value()) {
        auto counts = pending_counts_->build();
        if (!counts.has_value()) {
            return std::unexpected(std::move(counts.error()));
        }
        final_value_counts_ = std::make_shared<const PackedLongValues>(std::move(counts.value()));
    }
    return docs_with_field_.finish();
}

VoidResult SortedNumericDocValuesWriter::flush(DocValuesConsumer &consumer) {
    if (final_values_ == nullptr) {
        if (auto finished = finish(); !finished.has_value()) {
            return finished;
        }
    }
    SortedNumericDocValuesProducer producer(field_info_, final_values_, final_value_counts_, docs_with_field_);
    return consumer.add_sorted_numeric_field(*field_info_, producer);
}

} // namespace lucene
