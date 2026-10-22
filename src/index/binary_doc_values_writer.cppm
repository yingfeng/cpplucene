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

export module lucene_core:binary_doc_values_writer;

import :types;
import std;

import :binary_doc_values;
import :bytes_ref;
import :doc_id_set_iterator;
import :doc_values_consumer;
import :doc_values_producer;
import :doc_values_writer;
import :docs_with_field_set;
import :error;
import :field_infos;

namespace lucene {

/// [`BinaryDocValues`] over the buffered byte arrays.
///
/// Port of `BufferedBinaryDocValues` (`index/binary_doc_values_writer.rs`).
export class BufferedBinaryDocValues final : public BinaryDocValues {
public:
    BufferedBinaryDocValues(std::shared_ptr<const std::vector<BytesRef>> values,
                            std::unique_ptr<DocIdSetIterator> docs_with_field);

    [[nodiscard]] int32_t doc_id() const override { return docs_with_field_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<BytesRef> binary_value() override;

private:
    std::shared_ptr<const std::vector<BytesRef>> values_;
    std::unique_ptr<DocIdSetIterator> docs_with_field_;
    size_t index_{0};
    BytesRef value_;
};

/// [`DocValuesProducer`] over the buffered binary values.
///
/// Port of `DocValuesProducerImpl` (`index/binary_doc_values_writer.rs`),
/// without the index-sorting branch.
export class BinaryDocValuesProducer final : public DocValuesProducer {
public:
    BinaryDocValuesProducer(std::shared_ptr<FieldInfo> field_info,
                            std::shared_ptr<const std::vector<BytesRef>> values,
                            const DocsWithFieldSet &docs_with_field);

    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary(const FieldInfo &field) const override;
    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

private:
    std::shared_ptr<FieldInfo> field_info_;
    std::shared_ptr<const std::vector<BytesRef>> values_;
    const DocsWithFieldSet *docs_with_field_{nullptr};
};

/// Buffers one byte array per document and writes a binary doc-values field.
///
/// Port of `BinaryDocValuesWriter` (`index/binary_doc_values_writer.rs`).
///
/// **Deviations:**
///  * the reference implementation stores the bytes in a `PagedBytes` block structure; this port
///    keeps one `BytesRef` per value.  Same bytes, same order, no on-disk
///    difference -- only the RAM layout differs (and `PagedBytes` would only
///    matter for very large segments);
///  * no `iw_bytes_used` accounting.
export class BinaryDocValuesWriter final : public DocValuesWriter {
public:
    explicit BinaryDocValuesWriter(std::shared_ptr<FieldInfo> field_info);

    /// Appends the value of `doc_id`, which must be strictly increasing.
    [[nodiscard]] VoidResult add_value(int32_t doc_id, std::span<const uint8_t> value);

    [[nodiscard]] VoidResult finish() override;
    [[nodiscard]] VoidResult flush(DocValuesConsumer &consumer) override;

    [[nodiscard]] int32_t num_docs_with_field() const noexcept { return docs_with_field_.cardinality(); }
    [[nodiscard]] size_t max_length() const noexcept { return max_length_; }
    [[nodiscard]] const std::shared_ptr<FieldInfo> &field_info() const noexcept { return field_info_; }

private:
    std::shared_ptr<FieldInfo> field_info_;
    std::vector<BytesRef> values_;
    std::shared_ptr<const std::vector<BytesRef>> final_values_;
    DocsWithFieldSet docs_with_field_;
    size_t max_length_{0};
    int32_t last_doc_id_{-1};
};

} // namespace lucene
