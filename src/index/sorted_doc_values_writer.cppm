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

export module lucene_core:sorted_doc_values_writer;

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
import :sorted_doc_values;

namespace lucene {

/// [`SortedDocValues`] over a dictionary plus the per-document ordinals.
///
/// Port of `BufferedSortedDocValues` (`index/sorted_doc_values_writer.rs`).
export class BufferedSortedDocValues final : public SortedDocValues {
public:
    BufferedSortedDocValues(std::shared_ptr<const std::vector<BytesRef>> terms,
                            std::shared_ptr<const PackedLongValues> ords,
                            std::unique_ptr<DocIdSetIterator> docs_with_field);

    [[nodiscard]] int32_t doc_id() const override { return docs_with_field_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;

    [[nodiscard]] Result<int32_t> ord_value() override { return ord_; }
    [[nodiscard]] Result<BytesRef> lookup_ord(int32_t ord) override;
    [[nodiscard]] Result<int32_t> get_value_count() const override;

private:
    [[nodiscard]] VoidResult read_ord();

    std::shared_ptr<const std::vector<BytesRef>> terms_;
    std::shared_ptr<const PackedLongValues> ords_;
    std::unique_ptr<PackedLongValuesIterator> ords_iterator_;
    std::unique_ptr<DocIdSetIterator> docs_with_field_;
    int32_t ord_{-1};
};

/// [`DocValuesProducer`] over the buffered sorted values.
export class SortedDocValuesProducer final : public DocValuesProducer {
public:
    SortedDocValuesProducer(std::shared_ptr<FieldInfo> field_info,
                            std::shared_ptr<const std::vector<BytesRef>> terms,
                            std::shared_ptr<const PackedLongValues> ords,
                            const DocsWithFieldSet &docs_with_field);

    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const override;
    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

private:
    std::shared_ptr<FieldInfo> field_info_;
    std::shared_ptr<const std::vector<BytesRef>> terms_;
    std::shared_ptr<const PackedLongValues> ords_;
    const DocsWithFieldSet *docs_with_field_{nullptr};
};

/// Buffers one term per document and writes a sorted doc-values field.
///
/// Port of `SortedDocValuesWriter` (`index/sorted_doc_values_writer.rs`).
///
/// **Deviations:** the reference implementation collects the terms in a `BytesRefArray` (a
/// `ByteBlockPool` of blocks with a sortable iterator); this port buffers
/// `BytesRef`s and sorts them with `std::sort`, which produces the same
/// dictionary.  Index sorting (`sort_map`) and the `get_values` /
/// `get_terms_enum` helpers are not ported because they need `DocMap`.
export class SortedDocValuesWriter final : public DocValuesWriter {
public:
    explicit SortedDocValuesWriter(std::shared_ptr<FieldInfo> field_info);

    /// Appends the value of `doc_id`, which must be strictly increasing.
    [[nodiscard]] VoidResult add_value(int32_t doc_id, std::span<const uint8_t> value);

    [[nodiscard]] VoidResult finish() override;
    [[nodiscard]] VoidResult flush(DocValuesConsumer &consumer) override;

    [[nodiscard]] int32_t num_docs_with_field() const noexcept { return docs_with_field_.cardinality(); }
    [[nodiscard]] const std::shared_ptr<FieldInfo> &field_info() const noexcept { return field_info_; }

private:
    std::shared_ptr<FieldInfo> field_info_;
    std::vector<std::pair<int32_t, BytesRef>> pending_;
    DocsWithFieldSet docs_with_field_;
    std::shared_ptr<const std::vector<BytesRef>> final_terms_;
    std::shared_ptr<const PackedLongValues> final_ords_;
    int32_t last_doc_id_{-1};
};

} // namespace lucene
