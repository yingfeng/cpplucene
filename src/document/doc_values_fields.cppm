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

export module lucene_core:doc_values_fields;

import :types;
import std;

import :bytes_ref;
import :error;
import :field_type;
import :index_types;
import :indexable_field;
import :sorted_numeric_doc_values_range_query;
import :sorted_set_doc_values_range_query;

namespace lucene {

/// A numeric doc values field: one `int64` per document, not indexed or stored.
///
/// Port of `NumericDocValuesField` (`core/document/numeric_doc_values_field.rs`).
export struct NumericDocValuesField {
    /// Adds a new doc values field.
    ///
    /// **Deviation:** the reference implementation also exposes
    /// `set_long_value`/`numeric_value` on the field object; the port's field
    /// factories return a plain [`Field`], so the value is set at creation and can
    /// be changed with [`Field::set_i64_value`].
    [[nodiscard]] static Result<Field> create(std::string name, int64_t value);
};

/// A float doc values field: the raw `float` bits sign-extended into an `int64`.
///
/// Port of `FloatDocValuesField` (`core/document/float_doc_values_field.rs`).
export struct FloatDocValuesField {
    [[nodiscard]] static Result<Field> create(std::string name, float value);
    [[nodiscard]] static VoidResult set_value(Field &field, float value);
};

/// A double doc values field: the raw `double` bits as an `int64`.
///
/// Port of `DoubleDocValuesField` (`core/document/double_doc_values_field.rs`).
export struct DoubleDocValuesField {
    [[nodiscard]] static Result<Field> create(std::string name, double value);
    [[nodiscard]] static VoidResult set_value(Field &field, double value);
};

/// A single-valued binary doc values field.
///
/// Port of `BinaryDocValuesField` (`core/document/binary_doc_values_field.rs`).
export struct BinaryDocValuesField {
    [[nodiscard]] static Result<Field> create(std::string name, std::vector<uint8_t> value);
    /// Replaces the value of an existing field; the field type must be binary doc
    /// values.
    [[nodiscard]] static VoidResult set_value(Field &field, std::vector<uint8_t> value);
};

/// A single-valued sorted doc values field: the term is the value, ordinals are
/// assigned in dictionary order.
///
/// Port of `SortedDocValuesField` (`core/document/sorted_doc_values_field.rs`).
export struct SortedDocValuesField {
    [[nodiscard]] static Result<Field> create(std::string name, std::string value);
    [[nodiscard]] static VoidResult set_value(Field &field, std::string value);
};

/// A multi-valued sorted set doc values field; add one field per value.
///
/// Port of `SortedSetDocValuesField` (`core/document/sorted_set_doc_values_field.rs`).
export struct SortedSetDocValuesField {
    [[nodiscard]] static Result<Field> create(std::string name, std::string value);
    [[nodiscard]] static Result<Field> create(std::string name, std::string value,
                                              DocValuesSkipIndexType skip_index);
    [[nodiscard]] static VoidResult set_value(Field &field, std::string value);

    /// A query over the values of `field` in `[lower_value, upper_value]`.
    ///
    /// Port of `SortedSetDocValuesField::new_slow_range_query`: it reads doc values
    /// instead of postings, so an empty bound means "unbounded".
    [[nodiscard]] static Result<SortedSetDocValuesRangeQuery> new_slow_range_query(
            std::string field, std::optional<BytesRef> lower_value, std::optional<BytesRef> upper_value,
            bool lower_inclusive, bool upper_inclusive);

    /// A query for the documents that have exactly `value`.
    ///
    /// Port of `SortedSetDocValuesField::new_slow_exact_query`.
    [[nodiscard]] static Result<SortedSetDocValuesRangeQuery>
    new_slow_exact_query(std::string field, std::optional<BytesRef> value);
};

/// A multi-valued numeric doc values field; add one field per value.
///
/// Port of `SortedNumericDocValuesField`
/// (`core/document/sorted_numeric_doc_values_field.rs`).
export struct SortedNumericDocValuesField {
    [[nodiscard]] static Result<Field> create(std::string name, int64_t value);
    [[nodiscard]] static Result<Field> create(std::string name, int64_t value,
                                              DocValuesSkipIndexType skip_index);
    [[nodiscard]] static VoidResult set_value(Field &field, int64_t value);

    /// A query over the values of `field` in `[lower_value, upper_value]`.
    ///
    /// Port of `SortedNumericDocValuesField::new_slow_range_query`: it reads doc
    /// values instead of postings.
    [[nodiscard]] static Result<SortedNumericDocValuesRangeQuery> new_slow_range_query(std::string field,
                                                                                      int64_t lower_value,
                                                                                      int64_t upper_value);
};

} // namespace lucene
