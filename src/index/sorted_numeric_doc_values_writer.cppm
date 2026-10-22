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

export module lucene_core:sorted_numeric_doc_values_writer;

import :types;
import std;

import :doc_id_set_iterator;
import :doc_values_consumer;
import :doc_values_producer;
import :doc_values_writer;
import :docs_with_field_set;
import :error;
import :field_infos;
import :numeric_doc_values;
import :packed_long_values;
import :sorted_numeric_doc_values;

namespace lucene {

/// [`SortedNumericDocValues`] over a flat value stream plus per-doc counts.
///
/// Port of `BufferedSortedNumericDocValues`
/// (`index/sorted_numeric_doc_values_writer.rs`).
export class BufferedSortedNumericDocValues final : public SortedNumericDocValues {
public:
    BufferedSortedNumericDocValues(std::shared_ptr<const PackedLongValues> values,
                                   std::shared_ptr<const PackedLongValues> value_counts,
                                   std::unique_ptr<DocIdSetIterator> docs_with_field);

    [[nodiscard]] int32_t doc_id() const override { return docs_with_field_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<int64_t> next_value() override;
    [[nodiscard]] Result<int32_t> doc_value_count() override { return count_; }

private:
    /// Moves to `doc` (already returned by the docs iterator) and loads its
    /// value count.
    [[nodiscard]] VoidResult enter_doc();

    std::shared_ptr<const PackedLongValues> values_;
    std::shared_ptr<const PackedLongValues> value_counts_;
    std::unique_ptr<PackedLongValuesIterator> values_iterator_;
    std::unique_ptr<PackedLongValuesIterator> counts_iterator_;
    std::unique_ptr<DocIdSetIterator> docs_with_field_;
    int32_t count_{0};
    int32_t upto_{0};
};

/// [`SortedNumericDocValues`] with exactly one value per document.
///
/// Port of `SingletonSortedNumericDocValues`
/// (`index/singleton_sorted_numeric_doc_values.rs`).  `get_numeric_doc_values`
/// transfers the wrapped iterator, which is why the class is not reusable after
/// that call (same contract as the reference implementation's `enum` return).
export class SingletonSortedNumericDocValues final : public SortedNumericDocValues {
public:
    explicit SingletonSortedNumericDocValues(std::unique_ptr<NumericDocValues> inner)
            : inner_(std::move(inner)) {}

    [[nodiscard]] int32_t doc_id() const override { return inner_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return inner_->next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return inner_->advance(target); }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return inner_->slow_advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return inner_->cost(); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return inner_->advance_exact(target); }

    [[nodiscard]] Result<int64_t> next_value() override { return inner_->long_value(); }
    [[nodiscard]] Result<int32_t> doc_value_count() override { return 1; }
    [[nodiscard]] bool is_single_valued() const override { return true; }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values() override {
        if (inner_ == nullptr) {
            return err(LuceneError::illegal_state("the wrapped numeric doc values were already taken"));
        }
        return std::move(inner_);
    }

private:
    std::unique_ptr<NumericDocValues> inner_;
};

/// [`DocValuesProducer`] over the buffered sorted numeric values.
export class SortedNumericDocValuesProducer final : public DocValuesProducer {
public:
    SortedNumericDocValuesProducer(std::shared_ptr<FieldInfo> field_info,
                                   std::shared_ptr<const PackedLongValues> values,
                                   std::shared_ptr<const PackedLongValues> value_counts,
                                   const DocsWithFieldSet &docs_with_field);

    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric(
            const FieldInfo &field) const override;
    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

private:
    std::shared_ptr<FieldInfo> field_info_;
    std::shared_ptr<const PackedLongValues> values_;
    std::shared_ptr<const PackedLongValues> value_counts_;
    const DocsWithFieldSet *docs_with_field_{nullptr};
};

/// Buffers the (sorted) values of every document and writes a
/// sorted-numeric doc-values field.
///
/// Port of `SortedNumericDocValuesWriter`
/// (`index/sorted_numeric_doc_values_writer.rs`).
///
/// **Deviation:** no `iw_bytes_used` accounting.  When every document has a
/// single value the writer omits the count stream and emits the
/// single-valued shape, exactly like the reference implementation.
export class SortedNumericDocValuesWriter final : public DocValuesWriter {
public:
    explicit SortedNumericDocValuesWriter(std::shared_ptr<FieldInfo> field_info);

    /// Appends `value` to `doc_id`; documents must not go backwards.
    [[nodiscard]] VoidResult add_value(int32_t doc_id, int64_t value);

    [[nodiscard]] VoidResult finish() override;
    [[nodiscard]] VoidResult flush(DocValuesConsumer &consumer) override;

    [[nodiscard]] int32_t num_docs_with_field() const noexcept { return docs_with_field_.cardinality(); }
    [[nodiscard]] const std::shared_ptr<FieldInfo> &field_info() const noexcept { return field_info_; }

private:
    /// Sorts and appends the buffered values of the current document.
    [[nodiscard]] VoidResult finish_current_doc();

    std::shared_ptr<FieldInfo> field_info_;
    PackedLongValues::Builder pending_;
    std::optional<PackedLongValues::Builder> pending_counts_;
    DocsWithFieldSet docs_with_field_;
    int32_t current_doc_{-1};
    std::vector<int64_t> current_values_;
    size_t current_upto_{0};
    std::shared_ptr<const PackedLongValues> final_values_;
    std::shared_ptr<const PackedLongValues> final_value_counts_;
};

} // namespace lucene
