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

module lucene_core:field_kinds.impl;

import :field_kinds;
import :types;
import :error;
import :index_types;
import :field_type;
import :index_or_doc_values_query;
import :index_sort_sorted_numeric_doc_values_range_query;
import :numeric_utils;
import :point_fields;
import :query;
import :sorted_numeric_doc_values_range_query;
import :indexable_field;

import std;

namespace lucene {

namespace {

/// The doc-values half of a numeric range: the values are stored as `int64`, so
/// the caller passes the encoded bounds.
Result<std::shared_ptr<Query>> doc_values_range(const std::string &field, int64_t lower, int64_t upper) {
    auto built = SortedNumericDocValuesRangeQuery::create(field, lower, upper);
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<SortedNumericDocValuesRangeQuery>(std::move(built.value())));
}

/// `index` and `doc_values` both answer the same documents; the index is usually
/// cheaper on an index-sorted segment, the doc values on a random one.
Result<std::shared_ptr<Query>> index_or_doc_values(std::shared_ptr<Query> index, std::shared_ptr<Query> doc_values) {
    auto built = IndexOrDocValuesQuery::create(std::move(index), std::move(doc_values));
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<IndexOrDocValuesQuery>(std::move(built.value())));
}

/// The integer range queries: the index-sort shortcut is only tried for values
/// that are stored as they are, which is what the integer fields do.
Result<std::shared_ptr<Query>> integer_range(std::string field, int64_t lower, int64_t upper,
                                             std::shared_ptr<Query> index) {
    auto doc_values = doc_values_range(field, lower, upper);
    if (!doc_values.has_value()) {
        return std::unexpected(std::move(doc_values.error()));
    }
    auto fallback = index_or_doc_values(std::move(index), std::move(doc_values.value()));
    if (!fallback.has_value()) {
        return std::unexpected(std::move(fallback.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<IndexSortSortedNumericDocValuesRangeQuery>(
            std::move(field), lower, upper, std::move(fallback.value())));
}

} // namespace


Result<Field> TextField::create(std::string name, std::string text, bool stored) {
    FieldType type;
    std::ignore = type.set_stored(stored);
    std::ignore = type.set_tokenized(true);
    std::ignore = type.set_index_options(IndexOptions::DocsAndFreqsAndPositions);
    type.freeze();
    return Field::from_string(std::move(name), std::move(text), std::move(type));
}

Result<Field> StringField::create(std::string name, std::string value, bool stored) {
    FieldType type;
    std::ignore = type.set_stored(stored);
    std::ignore = type.set_tokenized(false);
    std::ignore = type.set_omit_norms(true);
    std::ignore = type.set_index_options(IndexOptions::Docs);
    type.freeze();
    return Field::from_string(std::move(name), std::move(value), std::move(type));
}

Result<Field> KeywordField::create(std::string name, std::string value, bool stored) {
    return KeywordField::create(std::move(name), std::move(value), stored, DocValuesSkipIndexType::None);
}

Result<Field> KeywordField::create(std::string name,
                                  std::string value,
                                   bool stored,
                                   DocValuesSkipIndexType skip_index) {
    FieldType type;
    std::ignore = type.set_stored(stored);
    std::ignore = type.set_tokenized(false);
    std::ignore = type.set_omit_norms(true);
    std::ignore = type.set_index_options(IndexOptions::Docs);
    std::ignore = type.set_doc_values_type(DocValuesType::SortedSet);
    std::ignore = type.set_doc_values_skip_index_type(skip_index);
    type.freeze();
    // Lucene's `KeywordField` holds a `BytesRef`, not a `String`: the sorted-set
    // doc values writer (and the binary inversion path) read `binaryValue()`.
    std::vector<uint8_t> bytes(value.begin(), value.end());
    return Field::from_bytes(std::move(name), std::move(bytes), std::move(type));
}

namespace {

FieldType numeric_type(bool stored, size_t num_bytes) {
    FieldType type;
    std::ignore = type.set_stored(stored);
    std::ignore = type.set_tokenized(false);
    std::ignore = type.set_doc_values_type(DocValuesType::Numeric);
    std::ignore = type.set_dimensions(1, num_bytes);
    type.freeze();
    return type;
}

} // namespace

Result<Field> IntField::create(std::string name, int32_t value, bool stored) {
    return Field::from_i32(std::move(name), value, numeric_type(stored, 4));
}

Result<Field> LongField::create(std::string name, int64_t value, bool stored) {
    return Field::from_i64(std::move(name), value, numeric_type(stored, 8));
}

Result<Field> FloatField::create(std::string name, float value, bool stored) {
    return Field::from_f32(std::move(name), value, numeric_type(stored, 4));
}

Result<Field> DoubleField::create(std::string name, double value, bool stored) {
    return Field::from_f64(std::move(name), value, numeric_type(stored, 8));
}

Result<std::shared_ptr<Query>> IntField::new_range_query(std::string field, int32_t lower_value,
                                                        int32_t upper_value) {
    auto index = IntPoint::new_range_query(field, lower_value, upper_value);
    if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
    }
    return integer_range(std::move(field), lower_value, upper_value, std::move(index.value()));
}

Result<std::shared_ptr<Query>> IntField::new_exact_query(std::string field, int32_t value) {
    return new_range_query(std::move(field), value, value);
}

Result<std::shared_ptr<Query>> LongField::new_range_query(std::string field, int64_t lower_value,
                                                          int64_t upper_value) {
    auto index = LongPoint::new_range_query(field, lower_value, upper_value);
    if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
    }
    return integer_range(std::move(field), lower_value, upper_value, std::move(index.value()));
}

Result<std::shared_ptr<Query>> LongField::new_exact_query(std::string field, int64_t value) {
    return new_range_query(std::move(field), value, value);
}

Result<std::shared_ptr<Query>> FloatField::new_range_query(std::string field, float lower_value, float upper_value) {
    auto index = FloatPoint::new_range_query(field, lower_value, upper_value);
    if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
    }
    // The doc values of a float field hold the sortable encoding of the value.
    auto doc_values = doc_values_range(field, NumericUtils::float_to_sortable_int(lower_value),
                                       NumericUtils::float_to_sortable_int(upper_value));
    if (!doc_values.has_value()) {
        return std::unexpected(std::move(doc_values.error()));
    }
    return index_or_doc_values(std::move(index.value()), std::move(doc_values.value()));
}

Result<std::shared_ptr<Query>> FloatField::new_exact_query(std::string field, float value) {
    return new_range_query(std::move(field), value, value);
}

Result<std::shared_ptr<Query>> DoubleField::new_range_query(std::string field, double lower_value, double upper_value) {
    auto index = DoublePoint::new_range_query(field, lower_value, upper_value);
    if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
    }
    auto doc_values = doc_values_range(field, NumericUtils::double_to_sortable_long(lower_value),
                                       NumericUtils::double_to_sortable_long(upper_value));
    if (!doc_values.has_value()) {
        return std::unexpected(std::move(doc_values.error()));
    }
    return index_or_doc_values(std::move(index.value()), std::move(doc_values.value()));
}

Result<std::shared_ptr<Query>> DoubleField::new_exact_query(std::string field, double value) {
    return new_range_query(std::move(field), value, value);
}

} // namespace lucene
