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
import :indexable_field;

import std;

namespace lucene {

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
    return Field::from_string(std::move(name), std::move(value), std::move(type));
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

} // namespace lucene
