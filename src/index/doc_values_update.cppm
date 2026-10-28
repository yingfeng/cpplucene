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

export module lucene_core:doc_values_update;

import :types;
import std;

import :bytes_ref;
import :error;
import :index_types;
import :term;

namespace lucene {

/// An in-place update of a numeric doc values field.
///
/// Port of `NumericDocValuesUpdate`
/// (`index/doc_values_update.rs`).  A "no value" update removes the value from
/// the document, like Lucene's `updateNumericDocValue(term, field, null)`.
export class NumericDocValuesUpdate {
public:
    /// An update that sets `value`.
    [[nodiscard]] static NumericDocValuesUpdate of(int64_t value);
    /// An update that removes the value.
    [[nodiscard]] static NumericDocValuesUpdate no_value();

    [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
    /// The value to set; `illegal_state` for a "no value" update.
    [[nodiscard]] Result<int64_t> value() const;
    [[nodiscard]] std::string value_to_string() const;

    [[nodiscard]] bool operator==(const NumericDocValuesUpdate &other) const noexcept {
        return value_ == other.value_;
    }

private:
    explicit NumericDocValuesUpdate(std::optional<int64_t> value) noexcept : value_(value) {}

    std::optional<int64_t> value_;
};

/// An in-place update of a binary doc values field.
///
/// Port of `BinaryDocValuesUpdate` (`index/doc_values_update.rs`).
export class BinaryDocValuesUpdate {
public:
    /// An update that sets `value`.
    [[nodiscard]] static BinaryDocValuesUpdate of(BytesRef value);
    /// An update that removes the value.
    [[nodiscard]] static BinaryDocValuesUpdate no_value();

    [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
    /// The value to set; `illegal_state` for a "no value" update.
    [[nodiscard]] Result<BytesRef> value() const;
    [[nodiscard]] std::string value_to_string() const;

    [[nodiscard]] bool operator==(const BinaryDocValuesUpdate &other) const noexcept {
        return value_ == other.value_;
    }

private:
    explicit BinaryDocValuesUpdate(std::optional<BytesRef> value) noexcept : value_(std::move(value)) {}

    std::optional<BytesRef> value_;
};

/// The new value of a doc values update.
export using DocValuesUpdateValue = std::variant<NumericDocValuesUpdate, BinaryDocValuesUpdate>;

/// An in-place update to a doc values field: the term selecting the documents,
/// the field and the new value.
///
/// Port of `DocValuesUpdate` (`index/doc_values_update.rs`).  `doc_id_upto` is
/// the number of documents the index held when the update was buffered; later
/// documents are not touched.
export class DocValuesUpdate {
public:
    /// An update with a term.
    [[nodiscard]] static DocValuesUpdate create(DocValuesType doc_values_type, Term term, std::string field,
                                               int32_t doc_id_upto, DocValuesUpdateValue value);
    /// An update whose term is optional (used when applying buffered updates).
    [[nodiscard]] static DocValuesUpdate with_optional_term(DocValuesType doc_values_type,
                                                           std::optional<Term> term, std::string field,
                                                           int32_t doc_id_upto, DocValuesUpdateValue value);

    [[nodiscard]] DocValuesType doc_values_type() const noexcept { return doc_values_type_; }
    [[nodiscard]] const std::optional<Term> &term() const noexcept { return term_; }
    /// The term of a buffered update; `illegal_state` when there is none.
    [[nodiscard]] Result<const Term *> buffered_term() const;
    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] int32_t doc_id_upto() const noexcept { return doc_id_upto_; }
    [[nodiscard]] bool has_value() const;
    [[nodiscard]] const DocValuesUpdateValue &sub_update() const noexcept { return sub_update_; }
    /// The numeric payload; `illegal_state` unless this is a numeric update.
    [[nodiscard]] Result<const NumericDocValuesUpdate *> numeric_update() const;
    /// The binary payload; `illegal_state` unless this is a binary update.
    [[nodiscard]] Result<const BinaryDocValuesUpdate *> binary_update() const;

    [[nodiscard]] std::string to_string() const;

private:
    DocValuesUpdate(DocValuesType doc_values_type, std::optional<Term> term, std::string field,
                    int32_t doc_id_upto, DocValuesUpdateValue value)
        : doc_values_type_(doc_values_type),
          term_(std::move(term)),
          field_(std::move(field)),
          doc_id_upto_(doc_id_upto),
          sub_update_(std::move(value)) {}

    DocValuesType doc_values_type_;
    std::optional<Term> term_;
    std::string field_;
    int32_t doc_id_upto_{0};
    DocValuesUpdateValue sub_update_;
};

} // namespace lucene
