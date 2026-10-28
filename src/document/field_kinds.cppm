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

export module lucene_core:field_kinds;

import :types;
import :error;

import std;

import :field_type;
import :index_or_doc_values_query;
import :index_sort_sorted_numeric_doc_values_range_query;
import :indexable_field;
import :numeric_utils;
import :point_fields;
import :query;
import :sorted_numeric_doc_values_range_query;

namespace lucene {

/// Full-text field: analyzed, positions indexed, optionally stored.
/// Port of `document/text_field.rs`.
export struct TextField {
    [[nodiscard]] static Result<Field> create(std::string name, std::string text, bool stored);
};

/// Exact-match field: indexed as a single term with norms omitted, optionally
/// stored.  Port of `document/string_field.rs`.
export struct StringField {
    [[nodiscard]] static Result<Field> create(std::string name, std::string value, bool stored);
};

/// Exact-match field with a `SortedSet` doc value, optionally stored
/// (`KeywordField`).
export struct KeywordField {
    [[nodiscard]] static Result<Field> create(std::string name, std::string value, bool stored);
    /// Variant with a custom skip-index setting.
    [[nodiscard]] static Result<Field> create(std::string name,
                                             std::string value,
                                             bool stored,
                                             DocValuesSkipIndexType skip_index);
};

/// Numeric fields indexed as points, with optional doc values and stored copy.
///
/// The range queries are the reference implementation's entry points for numeric
/// ranges: they answer an [`IndexOrDocValuesQuery`](lucene_core:index_or_doc_values_query)
/// over the points and the doc values, and the integer ones wrap it in an
/// [`IndexSortSortedNumericDocValuesRangeQuery`](lucene_core:index_sort_sorted_numeric_doc_values_range_query)
/// so that an index sorted by the field is searched by binary search.
///
/// **Deviation:** the queries are handed out as `shared_ptr<Query>` instead of the
/// concrete reference types, which is what every consumer in the port takes.
export struct IntField {
    [[nodiscard]] static Result<Field> create(std::string name, int32_t value, bool stored);

    /// The documents whose value is in `[lower_value, upper_value]`.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query(std::string field, int32_t lower_value,
                                                                      int32_t upper_value);
    /// The documents whose value is `value`.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_exact_query(std::string field, int32_t value);
};

export struct LongField {
    [[nodiscard]] static Result<Field> create(std::string name, int64_t value, bool stored);

    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query(std::string field, int64_t lower_value,
                                                                      int64_t upper_value);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_exact_query(std::string field, int64_t value);
};

export struct FloatField {
    [[nodiscard]] static Result<Field> create(std::string name, float value, bool stored);

    /// The doc values of a float field hold the sortable-int encoding of the
    /// value, which is what the range is translated into.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query(std::string field, float lower_value,
                                                                      float upper_value);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_exact_query(std::string field, float value);
};

export struct DoubleField {
    [[nodiscard]] static Result<Field> create(std::string name, double value, bool stored);

    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query(std::string field, double lower_value,
                                                                      double upper_value);
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_exact_query(std::string field, double value);
};

} // namespace lucene
