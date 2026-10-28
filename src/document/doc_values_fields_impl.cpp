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

module lucene_core:doc_values_fields.impl;

import :doc_values_fields;

import std;

import :error;
import :field_type;
import :index_types;
import :indexable_field;
import :types;

namespace lucene {

namespace {

/// A field type carrying only doc values of `type`.
[[nodiscard]] FieldType doc_values_type(DocValuesType type) {
    FieldType field_type;
    std::ignore = field_type.set_doc_values_type(type);
    field_type.freeze();
    return field_type;
}

/// A field type carrying doc values of `type` and a skip index.
[[nodiscard]] FieldType doc_values_type(DocValuesType type, DocValuesSkipIndexType skip_index) {
    FieldType field_type;
    std::ignore = field_type.set_doc_values_type(type);
    std::ignore = field_type.set_doc_values_skip_index_type(skip_index);
    field_type.freeze();
    return field_type;
}

} // namespace

Result<Field> NumericDocValuesField::create(std::string name, int64_t value) {
    return Field::from_i64(std::move(name), value, doc_values_type(DocValuesType::Numeric));
}

Result<Field> FloatDocValuesField::create(std::string name, float value) {
    const int32_t bits = std::bit_cast<int32_t>(value);
    return Field::from_i64(std::move(name), static_cast<int64_t>(bits), doc_values_type(DocValuesType::Numeric));
}

VoidResult FloatDocValuesField::set_value(Field &field, float value) {
    if (field.field_type().doc_values_type() != DocValuesType::Numeric) {
        return err(LuceneError::illegal_argument("the field is not a FloatDocValuesField"));
    }
    return field.set_i64_value(static_cast<int64_t>(std::bit_cast<int32_t>(value)));
}

Result<Field> DoubleDocValuesField::create(std::string name, double value) {
    return Field::from_i64(std::move(name), std::bit_cast<int64_t>(value), doc_values_type(DocValuesType::Numeric));
}

VoidResult DoubleDocValuesField::set_value(Field &field, double value) {
    if (field.field_type().doc_values_type() != DocValuesType::Numeric) {
        return err(LuceneError::illegal_argument("the field is not a DoubleDocValuesField"));
    }
    return field.set_i64_value(std::bit_cast<int64_t>(value));
}

Result<Field> BinaryDocValuesField::create(std::string name, std::vector<uint8_t> value) {
    return Field::from_bytes(std::move(name), std::move(value), doc_values_type(DocValuesType::Binary));
}

VoidResult BinaryDocValuesField::set_value(Field &field, std::vector<uint8_t> value) {
    if (field.field_type().doc_values_type() != DocValuesType::Binary) {
        return err(LuceneError::illegal_argument("the field is not a BinaryDocValuesField"));
    }
    return field.set_bytes_value(std::move(value));
}

Result<Field> SortedDocValuesField::create(std::string name, std::string value) {
    std::vector<uint8_t> bytes(value.begin(), value.end());
    return Field::from_bytes(std::move(name), std::move(bytes), doc_values_type(DocValuesType::Sorted));
}

VoidResult SortedDocValuesField::set_value(Field &field, std::string value) {
    if (field.field_type().doc_values_type() != DocValuesType::Sorted) {
        return err(LuceneError::illegal_argument("the field is not a SortedDocValuesField"));
    }
    return field.set_bytes_value(std::vector<uint8_t>(value.begin(), value.end()));
}

Result<Field> SortedSetDocValuesField::create(std::string name, std::string value) {
    return SortedSetDocValuesField::create(std::move(name), std::move(value), DocValuesSkipIndexType::None);
}

Result<Field> SortedSetDocValuesField::create(std::string name, std::string value,
                                              DocValuesSkipIndexType skip_index) {
    std::vector<uint8_t> bytes(value.begin(), value.end());
    return Field::from_bytes(std::move(name), std::move(bytes),
                             doc_values_type(DocValuesType::SortedSet, skip_index));
}

VoidResult SortedSetDocValuesField::set_value(Field &field, std::string value) {
    if (field.field_type().doc_values_type() != DocValuesType::SortedSet) {
        return err(LuceneError::illegal_argument("the field is not a SortedSetDocValuesField"));
    }
    return field.set_bytes_value(std::vector<uint8_t>(value.begin(), value.end()));
}

Result<Field> SortedNumericDocValuesField::create(std::string name, int64_t value) {
    return SortedNumericDocValuesField::create(std::move(name), value, DocValuesSkipIndexType::None);
}

Result<Field> SortedNumericDocValuesField::create(std::string name, int64_t value,
                                                  DocValuesSkipIndexType skip_index) {
    return Field::from_i64(std::move(name), value, doc_values_type(DocValuesType::SortedNumeric, skip_index));
}

VoidResult SortedNumericDocValuesField::set_value(Field &field, int64_t value) {
    if (field.field_type().doc_values_type() != DocValuesType::SortedNumeric) {
        return err(LuceneError::illegal_argument("the field is not a SortedNumericDocValuesField"));
    }
    return field.set_i64_value(value);
}

Result<SortedSetDocValuesRangeQuery> SortedSetDocValuesField::new_slow_range_query(
        std::string field, std::optional<BytesRef> lower_value, std::optional<BytesRef> upper_value,
        bool lower_inclusive, bool upper_inclusive) {
    return SortedSetDocValuesRangeQuery::create(std::move(field), std::move(lower_value), std::move(upper_value),
                                                lower_inclusive, upper_inclusive);
}

Result<SortedSetDocValuesRangeQuery> SortedSetDocValuesField::new_slow_exact_query(
        std::string field, std::optional<BytesRef> value) {
    return SortedSetDocValuesRangeQuery::create(std::move(field), value, value, true, true);
}

Result<SortedNumericDocValuesRangeQuery> SortedNumericDocValuesField::new_slow_range_query(std::string field,
                                                                                          int64_t lower_value,
                                                                                          int64_t upper_value) {
    return SortedNumericDocValuesRangeQuery::create(std::move(field), lower_value, upper_value);
}

} // namespace lucene
