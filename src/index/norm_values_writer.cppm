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

export module lucene_core:norm_values_writer;

import :types;
import std;

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

namespace lucene {

/// [`NumericDocValues`] over the buffered norms of a field.
///
/// Port of `BufferedNorms` (`index/norm_values_writer.rs`): the documents and
/// the values are stored in lock step, so every `next_doc` consumes one value.
export class BufferedNorms final : public NumericDocValues {
public:
    BufferedNorms(std::shared_ptr<const PackedLongValues> values,
                  std::unique_ptr<DocIdSetIterator> docs_with_field);

    [[nodiscard]] int32_t doc_id() const override { return docs_with_field_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override;
    /// Moves to the first document at or after `target`, consuming its value.
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

/// [`NormsProducer`] over the buffered norms.
///
/// Port of `NormsProducerImpl` (`index/norm_values_writer.rs`), without the
/// index-sorting branch.
export class NormsProducerImpl final : public NormsProducer {
public:
    NormsProducerImpl(std::shared_ptr<const PackedLongValues> values, const DocsWithFieldSet &docs_with_field);

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo &field) const override;
    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

private:
    std::shared_ptr<const PackedLongValues> values_;
    const DocsWithFieldSet *docs_with_field_{nullptr};
};

/// Buffers the norm of every document and writes the norms field.
///
/// Port of `NormValuesWriter` (`index/norm_values_writer.rs`).
///
/// **Deviation:** the reference implementation's `DocValuesWriter::flush` forwards to the
/// `NormsConsumer` it was built with.  In this port the writer owns no
/// consumer, so norms leave through [`flush_norms`] and the inherited
/// `flush(DocValuesConsumer&)` reports `unsupported_operation` (a norms field
/// must never be written as doc values).
export class NormValuesWriter final : public DocValuesWriter {
public:
    explicit NormValuesWriter(std::shared_ptr<FieldInfo> field_info);

    /// Appends the norm of `doc_id`, which must be strictly increasing.
    [[nodiscard]] VoidResult add_value(int32_t doc_id, int64_t value);

    [[nodiscard]] VoidResult finish() override;
    [[nodiscard]] VoidResult flush(DocValuesConsumer &consumer) override;

    /// Writes the buffered norms through the norms consumer.
    [[nodiscard]] VoidResult flush_norms(NormsConsumer &consumer);

    /// The buffered norms as a producer, for readers of the same buffers.
    ///
    /// The indexing chain needs this while flushing: the postings writer reads
    /// the norms of every document back to build its skip data, and they are
    /// still in memory at that point.
    [[nodiscard]] Result<std::unique_ptr<NormsProducer>> get_norms_producer();

    [[nodiscard]] int32_t num_docs_with_field() const noexcept { return docs_with_field_.cardinality(); }
    [[nodiscard]] const std::shared_ptr<FieldInfo> &field_info() const noexcept { return field_info_; }

private:
    std::shared_ptr<FieldInfo> field_info_;
    DocsWithFieldSet docs_with_field_;
    PackedLongValues::Builder pending_;
    std::shared_ptr<const PackedLongValues> final_values_;
    int32_t last_doc_id_{-1};
};

} // namespace lucene
