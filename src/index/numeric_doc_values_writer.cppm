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

export module lucene_core:numeric_doc_values_writer;

import :types;
import std;

import :doc_id_set_iterator;
import :doc_values_consumer;
import :doc_values_iterator;
import :doc_values_producer;
import :doc_values_writer;
import :docs_with_field_set;
import :error;
import :field_infos;
import :numeric_doc_values;
import :packed_long_values;

namespace lucene {

/// [`NumericDocValues`] over the buffered values plus the docs that have one.
///
/// Port of `BufferedNumericDocValues` (`index/numeric_doc_values_writer.rs`).
export class BufferedNumericDocValues final : public NumericDocValues {
public:
    BufferedNumericDocValues(std::shared_ptr<const PackedLongValues> values,
                             std::unique_ptr<DocIdSetIterator> docs_with_field);

    [[nodiscard]] int32_t doc_id() const override { return docs_with_field_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<int64_t> long_value() override { return value_; }

private:
    std::shared_ptr<const PackedLongValues> values_;
    std::unique_ptr<PackedLongValuesIterator> values_iterator_;
    std::unique_ptr<DocIdSetIterator> docs_with_field_;
    int64_t value_{0};
};

/// [`DocValuesProducer`] that hands out the buffered numeric values.
///
/// Port of `DocValuesProducerImpl` (`index/numeric_doc_values_writer.rs`),
/// without the index-sorting branch (no `DocMap` in this port).
export class NumericDocValuesProducer final : public DocValuesProducer {
public:
    NumericDocValuesProducer(std::shared_ptr<FieldInfo> field_info,
                             std::shared_ptr<const PackedLongValues> values,
                             const DocsWithFieldSet &docs_with_field);

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &field) const override;
    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

private:
    std::shared_ptr<FieldInfo> field_info_;
    std::shared_ptr<const PackedLongValues> values_;
    const DocsWithFieldSet *docs_with_field_{nullptr};
};

/// Buffers one `long` per document and writes a numeric doc-values field.
///
/// Port of `NumericDocValuesWriter` (`index/numeric_doc_values_writer.rs`).
///
/// **Deviation:** no `iw_bytes_used` accounting (no `Accountable`/`Counter` in
/// this port), and the values are kept in a [`PackedLongValues`] built at
/// [`finish`] rather than rebuilt per flush.
export class NumericDocValuesWriter final : public DocValuesWriter {
public:
    explicit NumericDocValuesWriter(std::shared_ptr<FieldInfo> field_info);

    /// Appends a value; `doc_id` must be strictly increasing.
    [[nodiscard]] VoidResult add_value(int32_t doc_id, int64_t value);

    [[nodiscard]] VoidResult finish() override;
    [[nodiscard]] VoidResult flush(DocValuesConsumer &consumer) override;

    [[nodiscard]] int32_t num_docs_with_field() const noexcept { return docs_with_field_.cardinality(); }
    [[nodiscard]] const std::shared_ptr<FieldInfo> &field_info() const noexcept { return field_info_; }

private:
    PackedLongValues::Builder pending_;
    std::shared_ptr<const PackedLongValues> final_values_;
    DocsWithFieldSet docs_with_field_;
    std::shared_ptr<FieldInfo> field_info_;
    int32_t last_doc_id_{-1};
};

} // namespace lucene
