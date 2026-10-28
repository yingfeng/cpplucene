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

export module lucene_core:doc_values_field_updates;

import :types;
import std;

import :bytes_ref;
import :error;
import :index_types;

namespace lucene {

/// Iterates the updates of one field, in ascending document order.
///
/// Port of the iterator half of `index/doc_values_field_updates.rs`: a numeric
/// batch yields [`long_value`], a binary batch yields [`binary_value`].  A
/// document whose value is removed yields nothing from the batch, so the writer
/// drops it.
export class DocValuesFieldUpdatesIterator {
public:
    virtual ~DocValuesFieldUpdatesIterator() = default;

    /// Advances to the next document with an update; `false` past the end.
    [[nodiscard]] virtual bool next() = 0;
    /// The current document.
    [[nodiscard]] virtual int32_t doc() const = 0;
    /// The numeric value of the current document.
    [[nodiscard]] virtual Result<int64_t> long_value() const = 0;
    /// The binary value of the current document.
    [[nodiscard]] virtual Result<BytesRef> binary_value() const = 0;
};

/// The buffered in-place updates of one field of one segment.
///
/// Port of `DocValuesFieldUpdates` (`index/doc_values_field_updates.rs`), the
/// common base of the numeric and binary implementations.  A document may be
/// updated to a value or to *no* value, which is how Lucene's
/// `updateNumericDocValue(term, field, null)` removes one.
///
/// **Deviation:** the reference implementation stores the documents and values
/// in the paged-mutable family (`PagedMutable` / `PagedGrowableWriter`) so a
/// huge batch stays compact; the port uses plain vectors, which hold the same
/// data for the batch sizes an index writer buffers.  The externally visible
/// behaviour — ascending document order, `finish` freezing the batch, the value
/// or its removal — is unchanged.
export class DocValuesFieldUpdates {
public:
    virtual ~DocValuesFieldUpdates() = default;

    /// The field these updates belong to.
    [[nodiscard]] virtual const std::string &field() const noexcept = 0;
    /// [`DocValuesType::Numeric`] or [`DocValuesType::Binary`].
    [[nodiscard]] virtual DocValuesType sub_type() const noexcept = 0;

    /// Sets (or removes, when `value` is empty) the value of `doc`.
    [[nodiscard]] virtual VoidResult add_numeric(int32_t doc, std::optional<int64_t> value) = 0;
    /// Sets (or removes, when `value` is empty) the value of `doc`.
    [[nodiscard]] virtual VoidResult add_binary(int32_t doc, std::optional<BytesRef> value) = 0;

    /// Sorts the batch by document and keeps the last update of each document.
    virtual void finish() = 0;
    /// Whether [`finish`] ran; iterating before it is an `illegal_state`.
    [[nodiscard]] virtual bool finished() const noexcept = 0;

    /// Number of updated documents.
    [[nodiscard]] virtual size_t size() const noexcept = 0;
    /// The bytes this batch holds, as reported by `ramBytesUsed`.
    [[nodiscard]] virtual int64_t ram_bytes_used() const = 0;

    /// `true` when the batch covers `doc`, with a value or with a removal.
    [[nodiscard]] virtual bool is_updated(int32_t doc) const = 0;
    /// The new value of `doc`; empty when it is removed or not covered.
    [[nodiscard]] virtual std::optional<int64_t> numeric_value(int32_t doc) const = 0;
    /// The new value of `doc`; empty when it is removed or not covered.
    [[nodiscard]] virtual std::optional<BytesRef> binary_value(int32_t doc) const = 0;

    /// Iterates the documents that keep a value, in ascending document order.
    [[nodiscard]] virtual std::unique_ptr<DocValuesFieldUpdatesIterator> iterator() const = 0;
};

/// The updates of a numeric doc values field.
export class NumericDocValuesFieldUpdates final : public DocValuesFieldUpdates {
public:
    /// `sub_type` is [`DocValuesType::Numeric`], or `SortedNumeric` for a field
    /// that stores several values per document.
    NumericDocValuesFieldUpdates(std::string field, DocValuesType sub_type);

    [[nodiscard]] const std::string &field() const noexcept override { return field_; }
    [[nodiscard]] DocValuesType sub_type() const noexcept override { return sub_type_; }

    [[nodiscard]] VoidResult add_numeric(int32_t doc, std::optional<int64_t> value) override;
    /// Unsupported: this batch only holds numeric values.
    [[nodiscard]] VoidResult add_binary(int32_t doc, std::optional<BytesRef> value) override;

    void finish() override;
    [[nodiscard]] bool finished() const noexcept override { return finished_; }

    [[nodiscard]] size_t size() const noexcept override { return docs_.size(); }
    [[nodiscard]] int64_t ram_bytes_used() const override;

    [[nodiscard]] bool is_updated(int32_t doc) const override;
    [[nodiscard]] std::optional<int64_t> numeric_value(int32_t doc) const override;
    /// Always empty: this batch only holds numeric values.
    [[nodiscard]] std::optional<BytesRef> binary_value(int32_t doc) const override;

    [[nodiscard]] std::unique_ptr<DocValuesFieldUpdatesIterator> iterator() const override;

    /// The documents, ascending once [`finish`] ran.
    [[nodiscard]] const std::vector<int32_t> &docs() const noexcept { return docs_; }
    /// Whether each document keeps a value, parallel to [`docs`].
    [[nodiscard]] const std::vector<uint8_t> &present() const noexcept { return present_; }
    /// The values, parallel to [`docs`]; only meaningful where [`present`] is 1.
    [[nodiscard]] const std::vector<int64_t> &values() const noexcept { return values_; }

private:
    std::string field_;
    DocValuesType sub_type_;
    std::vector<int32_t> docs_;
    std::vector<int64_t> values_;
    std::vector<uint8_t> present_;
    bool finished_{false};
};

/// The updates of a binary doc values field.
export class BinaryDocValuesFieldUpdates final : public DocValuesFieldUpdates {
public:
    /// `sub_type` is [`DocValuesType::Binary`], or `SortedSet` for a field that
    /// stores several values per document.
    BinaryDocValuesFieldUpdates(std::string field, DocValuesType sub_type);

    [[nodiscard]] const std::string &field() const noexcept override { return field_; }
    [[nodiscard]] DocValuesType sub_type() const noexcept override { return sub_type_; }

    /// Unsupported: this batch only holds binary values.
    [[nodiscard]] VoidResult add_numeric(int32_t doc, std::optional<int64_t> value) override;
    [[nodiscard]] VoidResult add_binary(int32_t doc, std::optional<BytesRef> value) override;

    void finish() override;
    [[nodiscard]] bool finished() const noexcept override { return finished_; }

    [[nodiscard]] size_t size() const noexcept override { return docs_.size(); }
    [[nodiscard]] int64_t ram_bytes_used() const override;

    [[nodiscard]] bool is_updated(int32_t doc) const override;
    /// Always empty: this batch only holds binary values.
    [[nodiscard]] std::optional<int64_t> numeric_value(int32_t doc) const override;
    [[nodiscard]] std::optional<BytesRef> binary_value(int32_t doc) const override;

    [[nodiscard]] std::unique_ptr<DocValuesFieldUpdatesIterator> iterator() const override;

    /// The documents, ascending once [`finish`] ran.
    [[nodiscard]] const std::vector<int32_t> &docs() const noexcept { return docs_; }
    /// Whether each document keeps a value, parallel to [`docs`].
    [[nodiscard]] const std::vector<uint8_t> &present() const noexcept { return present_; }
    /// The values, parallel to [`docs`]; only meaningful where [`present`] is 1.
    [[nodiscard]] const std::vector<BytesRef> &values() const noexcept { return values_; }

private:
    std::string field_;
    DocValuesType sub_type_;
    std::vector<int32_t> docs_;
    std::vector<BytesRef> values_;
    std::vector<uint8_t> present_;
    bool finished_{false};
};

} // namespace lucene
