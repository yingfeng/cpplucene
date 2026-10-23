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

export module lucene_core:sorting_producers;

import :types;
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

namespace lucene {

/// Norms presented in index-sorted order.
///
/// Port of the norms half of `sorting_codec_reader.rs`
/// (`core/index/sorting_codec_reader.rs`).
export class SortingNormsProducer final : public NormsProducer {
public:
    /// Wraps `producer`; both arguments must outlive the wrapper.
    [[nodiscard]] static Result<std::unique_ptr<SortingNormsProducer>> create(
            std::shared_ptr<NormsProducer> producer, std::shared_ptr<const IndexSortDocMap> doc_map);

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo &field) const override;
    [[nodiscard]] VoidResult check_integrity() const override;
    [[nodiscard]] Result<std::unique_ptr<NormsProducer>> get_merge_instance() const override;
    [[nodiscard]] VoidResult close() const override;

private:
    SortingNormsProducer(std::shared_ptr<NormsProducer> producer, std::shared_ptr<const IndexSortDocMap> doc_map);

    std::shared_ptr<NormsProducer> producer_;
    std::shared_ptr<const IndexSortDocMap> doc_map_;
};

/// Doc values presented in index-sorted order.
///
/// Port of the doc values producer half of `sorting_codec_reader.rs`
/// (`core/index/sorting_codec_reader.rs`); the per-field wrappers themselves live
/// in `:sorting_doc_values`.
export class SortingDocValuesProducer final : public DocValuesProducer {
public:
    /// Wraps `producer`; both arguments must outlive the wrapper.
    [[nodiscard]] static Result<std::unique_ptr<SortingDocValuesProducer>> create(
            std::shared_ptr<DocValuesProducer> producer, std::shared_ptr<const IndexSortDocMap> doc_map);

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary(const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric(
            const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set(const FieldInfo &field) const override;
    [[nodiscard]] VoidResult check_integrity() const override;
    [[nodiscard]] Result<std::unique_ptr<DocValuesProducer>> get_merge_instance() const override;
    [[nodiscard]] VoidResult close() const override;

private:
    SortingDocValuesProducer(std::shared_ptr<DocValuesProducer> producer,
                             std::shared_ptr<const IndexSortDocMap> doc_map);

    std::shared_ptr<DocValuesProducer> producer_;
    std::shared_ptr<const IndexSortDocMap> doc_map_;
};

} // namespace lucene
