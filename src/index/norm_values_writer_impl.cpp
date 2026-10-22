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

module lucene_core:norm_values_writer.impl;

import :norm_values_writer;

import :doc_id_set_iterator;
import :doc_values_consumer;
import :doc_values_writer;
import :docs_with_field_set;
import :error;
import :field_infos;
import :norms_consumer;
import :norms_producer;
import :numeric_doc_values;
import :packed_long_values;
import :types;

import std;

namespace lucene {

// ---------------------------------------------------------------------------
// BufferedNorms
// ---------------------------------------------------------------------------

BufferedNorms::BufferedNorms(std::shared_ptr<const PackedLongValues> values,
                             std::unique_ptr<DocIdSetIterator> docs_with_field)
        : values_(std::move(values)),
          values_iterator_(values_->iterator()),
          docs_with_field_(std::move(docs_with_field)) {}

Result<int32_t> BufferedNorms::next_doc() {
    auto doc = docs_with_field_->next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        auto value = values_iterator_->next();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        value_ = value.value();
    }
    return doc.value();
}

Result<int32_t> BufferedNorms::advance(int32_t target) {
    auto doc = docs_with_field_->advance(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        auto value = values_iterator_->next();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        value_ = value.value();
    }
    return doc.value();
}

Result<int64_t> BufferedNorms::cost() const { return docs_with_field_->cost(); }

Result<bool> BufferedNorms::advance_exact(int32_t target) {
    // The buffered values are addressed sequentially, so "exact" is an advance
    // followed by a comparison; the docs iterator never moves backwards.
    auto doc = advance(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    return doc.value() == target;
}

// ---------------------------------------------------------------------------
// NormsProducerImpl
// ---------------------------------------------------------------------------

NormsProducerImpl::NormsProducerImpl(std::shared_ptr<const PackedLongValues> values,
                                     const DocsWithFieldSet &docs_with_field)
        : values_(std::move(values)), docs_with_field_(&docs_with_field) {}

Result<std::unique_ptr<NumericDocValues>> NormsProducerImpl::get_norms(const FieldInfo & /*field*/) const {
    // The reference implementation ignores the field: the producer only ever holds one field's norms.
    auto docs = docs_with_field_->iterator();
    if (!docs.has_value()) {
        return std::unexpected(std::move(docs.error()));
    }
    return std::unique_ptr<NumericDocValues>(std::make_unique<BufferedNorms>(values_, std::move(docs.value())));
}

// ---------------------------------------------------------------------------
// NormValuesWriter
// ---------------------------------------------------------------------------

NormValuesWriter::NormValuesWriter(std::shared_ptr<FieldInfo> field_info)
        : field_info_(std::move(field_info)),
          pending_(PackedLongValues::DEFAULT_PAGE_SIZE, sizeof(int64_t) * 8, 0, true) {}

VoidResult NormValuesWriter::add_value(int32_t doc_id, int64_t value) {
    if (doc_id <= last_doc_id_) {
        return err(LuceneError::illegal_argument("Norm for \"" + field_info_->name +
                                                 "\" appears more than once in this document (only one value is "
                                                 "allowed per field)"));
    }
    if (auto added = pending_.add(value); !added.has_value()) {
        return added;
    }
    if (auto added = docs_with_field_.add(doc_id); !added.has_value()) {
        return added;
    }
    last_doc_id_ = doc_id;
    return {};
}

VoidResult NormValuesWriter::finish() {
    if (final_values_ == nullptr) {
        auto built = pending_.build();
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        final_values_ = std::make_shared<const PackedLongValues>(std::move(built.value()));
    }
    return docs_with_field_.finish();
}

VoidResult NormValuesWriter::flush(DocValuesConsumer & /*consumer*/) {
    return err(LuceneError::unsupported_operation(
            "norms are written through NormsConsumer, not as doc values; use flush_norms"));
}

Result<std::unique_ptr<NormsProducer>> NormValuesWriter::get_norms_producer() {
    if (final_values_ == nullptr) {
        if (auto finished = finish(); !finished.has_value()) {
            return std::unexpected(std::move(finished.error()));
        }
    }
    return std::unique_ptr<NormsProducer>(
            std::make_unique<NormsProducerImpl>(final_values_, docs_with_field_));
}

VoidResult NormValuesWriter::flush_norms(NormsConsumer &consumer) {
    if (final_values_ == nullptr) {
        if (auto finished = finish(); !finished.has_value()) {
            return finished;
        }
    }
    NormsProducerImpl producer(final_values_, docs_with_field_);
    return consumer.add_norms_field(*field_info_, producer);
}

} // namespace lucene
