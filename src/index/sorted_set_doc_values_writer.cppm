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

export module lucene_core:sorted_set_doc_values_writer;

import :types;
import std;

import :bytes_ref;
import :doc_id_set_iterator;
import :doc_values_consumer;
import :doc_values_producer;
import :doc_values_writer;
import :docs_with_field_set;
import :error;
import :field_infos;
import :packed_long_values;
import :sorted_set_doc_values;

namespace lucene {

/// [`SortedSetDocValues`] over a dictionary plus per-document ordinal runs.
///
/// Port of `BufferedSortedSetDocValues`
/// (`index/sorted_set_doc_values_writer.rs`).
export class BufferedSortedSetDocValues final : public SortedSetDocValues {
public:
    BufferedSortedSetDocValues(std::shared_ptr<const std::vector<BytesRef>> terms,
                               std::shared_ptr<const PackedLongValues> ords,
                               std::shared_ptr<const PackedLongValues> value_counts,
                               std::unique_ptr<DocIdSetIterator> docs_with_field);

    [[nodiscard]] int32_t doc_id() const override { return docs_with_field_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;

    [[nodiscard]] Result<int64_t> next_ord() override;
    [[nodiscard]] Result<int32_t> doc_value_count() override { return count_; }
    [[nodiscard]] Result<BytesRef> lookup_ord(int64_t ord) override;
    [[nodiscard]] Result<int64_t> get_value_count() const override;

private:
    [[nodiscard]] VoidResult read_count();

    std::shared_ptr<const std::vector<BytesRef>> terms_;
    std::shared_ptr<const PackedLongValues> ords_;
    std::shared_ptr<const PackedLongValues> value_counts_;
    std::unique_ptr<PackedLongValuesIterator> ords_iterator_;
    std::unique_ptr<PackedLongValuesIterator> counts_iterator_;
    std::unique_ptr<DocIdSetIterator> docs_with_field_;
    int32_t count_{0};
    int32_t upto_{0};
};

/// [`DocValuesProducer`] over the buffered sorted set values.
export class SortedSetDocValuesProducer final : public DocValuesProducer {
public:
    SortedSetDocValuesProducer(std::shared_ptr<FieldInfo> field_info,
                               std::shared_ptr<const std::vector<BytesRef>> terms,
                               std::shared_ptr<const PackedLongValues> ords,
                               std::shared_ptr<const PackedLongValues> value_counts,
                               const DocsWithFieldSet &docs_with_field);

    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set(const FieldInfo &field) const override;
    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

private:
    std::shared_ptr<FieldInfo> field_info_;
    std::shared_ptr<const std::vector<BytesRef>> terms_;
    std::shared_ptr<const PackedLongValues> ords_;
    std::shared_ptr<const PackedLongValues> value_counts_;
    const DocsWithFieldSet *docs_with_field_{nullptr};
};

/// Buffers the values of every document and writes a sorted-set doc-values
/// field.
///
/// Port of `SortedSetDocValuesWriter`
/// (`index/sorted_set_doc_values_writer.rs`).
///
/// **Deviations:** same as the single-valued writer: `BytesRef`s plus
/// `std::sort` instead of `BytesRefArray`, and no index-sorting path.
export class SortedSetDocValuesWriter final : public DocValuesWriter {
public:
    explicit SortedSetDocValuesWriter(std::shared_ptr<FieldInfo> field_info);

    /// Appends `value` to `doc_id`; documents must not go backwards.
    [[nodiscard]] VoidResult add_value(int32_t doc_id, std::span<const uint8_t> value);

    [[nodiscard]] VoidResult finish() override;
    [[nodiscard]] VoidResult flush(DocValuesConsumer &consumer) override;

    [[nodiscard]] int32_t num_docs_with_field() const noexcept { return docs_with_field_.cardinality(); }
    [[nodiscard]] const std::shared_ptr<FieldInfo> &field_info() const noexcept { return field_info_; }

private:
    std::shared_ptr<FieldInfo> field_info_;
    /// In insertion order; `doc_id` is non-decreasing.
    std::vector<std::pair<int32_t, BytesRef>> pending_;
    DocsWithFieldSet docs_with_field_;
    std::shared_ptr<const std::vector<BytesRef>> final_terms_;
    std::shared_ptr<const PackedLongValues> final_ords_;
    std::shared_ptr<const PackedLongValues> final_value_counts_;
    int32_t current_doc_{-1};
};

} // namespace lucene
