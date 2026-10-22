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
import :indexable_field;

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
export struct IntField {
    [[nodiscard]] static Result<Field> create(std::string name, int32_t value, bool stored);
};

export struct LongField {
    [[nodiscard]] static Result<Field> create(std::string name, int64_t value, bool stored);
};

export struct FloatField {
    [[nodiscard]] static Result<Field> create(std::string name, float value, bool stored);
};

export struct DoubleField {
    [[nodiscard]] static Result<Field> create(std::string name, double value, bool stored);
};

} // namespace lucene
