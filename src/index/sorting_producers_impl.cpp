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

module lucene_core:sorting_producers.impl;

import :sorting_producers;

import std;

import :binary_doc_values;
import :doc_values_producer;
import :error;
import :field_infos;
import :index_sorter;
import :norms_producer;
import :numeric_doc_values;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :sorting_doc_values;
import :types;

namespace lucene {

// ---- SortingNormsProducer ----

SortingNormsProducer::SortingNormsProducer(std::shared_ptr<NormsProducer> producer,
                                           std::shared_ptr<const IndexSortDocMap> doc_map)
        : producer_(std::move(producer)), doc_map_(std::move(doc_map)) {}

Result<std::unique_ptr<SortingNormsProducer>> SortingNormsProducer::create(
        std::shared_ptr<NormsProducer> producer, std::shared_ptr<const IndexSortDocMap> doc_map) {
    if (producer == nullptr || doc_map == nullptr) {
        return err(LuceneError::illegal_argument("the producer and the doc map must not be null"));
    }
    return std::unique_ptr<SortingNormsProducer>(new SortingNormsProducer(std::move(producer), std::move(doc_map)));
}

Result<std::unique_ptr<NumericDocValues>> SortingNormsProducer::get_norms(const FieldInfo &field) const {
    auto values = producer_->get_norms(field);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    if (values.value() == nullptr) {
        return std::unique_ptr<NumericDocValues>();
    }
    auto sorted = SortingNumericDocValues::create(std::move(values.value()), *doc_map_);
    if (!sorted.has_value()) {
        return std::unexpected(std::move(sorted.error()));
    }
    return std::unique_ptr<NumericDocValues>(std::move(sorted.value()));
}

VoidResult SortingNormsProducer::check_integrity() const {
    return producer_->check_integrity();
}

Result<std::unique_ptr<NormsProducer>> SortingNormsProducer::get_merge_instance() const {
    auto instance = producer_->get_merge_instance();
    if (!instance.has_value()) {
        return std::unexpected(std::move(instance.error()));
    }
    if (instance.value() == nullptr) {
        // The producer is already merge-friendly; wrap it directly.
        return std::unique_ptr<NormsProducer>(new SortingNormsProducer(producer_, doc_map_));
    }
    return create(std::shared_ptr<NormsProducer>(std::move(instance.value())), doc_map_);
}

VoidResult SortingNormsProducer::close() const {
    return producer_->close();
}

// ---- SortingDocValuesProducer ----

SortingDocValuesProducer::SortingDocValuesProducer(std::shared_ptr<DocValuesProducer> producer,
                                                   std::shared_ptr<const IndexSortDocMap> doc_map)
        : producer_(std::move(producer)), doc_map_(std::move(doc_map)) {}

Result<std::unique_ptr<SortingDocValuesProducer>> SortingDocValuesProducer::create(
        std::shared_ptr<DocValuesProducer> producer, std::shared_ptr<const IndexSortDocMap> doc_map) {
    if (producer == nullptr || doc_map == nullptr) {
        return err(LuceneError::illegal_argument("the producer and the doc map must not be null"));
    }
    return std::unique_ptr<SortingDocValuesProducer>(
            new SortingDocValuesProducer(std::move(producer), std::move(doc_map)));
}

Result<std::unique_ptr<NumericDocValues>> SortingDocValuesProducer::get_numeric(const FieldInfo &field) const {
    auto values = producer_->get_numeric(field);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    if (values.value() == nullptr) {
        return std::unique_ptr<NumericDocValues>();
    }
    auto sorted = SortingNumericDocValues::create(std::move(values.value()), *doc_map_);
    if (!sorted.has_value()) {
        return std::unexpected(std::move(sorted.error()));
    }
    return std::unique_ptr<NumericDocValues>(std::move(sorted.value()));
}

Result<std::unique_ptr<BinaryDocValues>> SortingDocValuesProducer::get_binary(const FieldInfo &field) const {
    auto values = producer_->get_binary(field);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    if (values.value() == nullptr) {
        return std::unique_ptr<BinaryDocValues>();
    }
    auto sorted = SortingBinaryDocValues::create(std::move(values.value()), *doc_map_);
    if (!sorted.has_value()) {
        return std::unexpected(std::move(sorted.error()));
    }
    return std::unique_ptr<BinaryDocValues>(std::move(sorted.value()));
}

Result<std::unique_ptr<SortedDocValues>> SortingDocValuesProducer::get_sorted(const FieldInfo &field) const {
    auto values = producer_->get_sorted(field);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    if (values.value() == nullptr) {
        return std::unique_ptr<SortedDocValues>();
    }
    auto sorted = SortingSortedDocValues::create(std::move(values.value()), *doc_map_);
    if (!sorted.has_value()) {
        return std::unexpected(std::move(sorted.error()));
    }
    return std::unique_ptr<SortedDocValues>(std::move(sorted.value()));
}

Result<std::unique_ptr<SortedNumericDocValues>> SortingDocValuesProducer::get_sorted_numeric(
        const FieldInfo &field) const {
    auto values = producer_->get_sorted_numeric(field);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    if (values.value() == nullptr) {
        return std::unique_ptr<SortedNumericDocValues>();
    }
    auto sorted = SortingSortedNumericDocValues::create(std::move(values.value()), *doc_map_);
    if (!sorted.has_value()) {
        return std::unexpected(std::move(sorted.error()));
    }
    return std::unique_ptr<SortedNumericDocValues>(std::move(sorted.value()));
}

Result<std::unique_ptr<SortedSetDocValues>> SortingDocValuesProducer::get_sorted_set(const FieldInfo &field) const {
    auto values = producer_->get_sorted_set(field);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    if (values.value() == nullptr) {
        return std::unique_ptr<SortedSetDocValues>();
    }
    auto sorted = SortingSortedSetDocValues::create(std::move(values.value()), *doc_map_);
    if (!sorted.has_value()) {
        return std::unexpected(std::move(sorted.error()));
    }
    return std::unique_ptr<SortedSetDocValues>(std::move(sorted.value()));
}

VoidResult SortingDocValuesProducer::check_integrity() const {
    return producer_->check_integrity();
}

Result<std::unique_ptr<DocValuesProducer>> SortingDocValuesProducer::get_merge_instance() const {
    auto instance = producer_->get_merge_instance();
    if (!instance.has_value()) {
        return std::unexpected(std::move(instance.error()));
    }
    if (instance.value() == nullptr) {
        return std::unique_ptr<DocValuesProducer>(new SortingDocValuesProducer(producer_, doc_map_));
    }
    return create(std::shared_ptr<DocValuesProducer>(std::move(instance.value())), doc_map_);
}

VoidResult SortingDocValuesProducer::close() const {
    return producer_->close();
}

} // namespace lucene
