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

module lucene_core:doc_values_update.impl;

import :doc_values_update;

import :bytes_ref;
import :error;
import :index_types;
import :term;
import :types;

import std;

namespace lucene {

// ---- NumericDocValuesUpdate ----

NumericDocValuesUpdate NumericDocValuesUpdate::of(int64_t value) {
    return NumericDocValuesUpdate(value);
}

NumericDocValuesUpdate NumericDocValuesUpdate::no_value() {
    return NumericDocValuesUpdate(std::nullopt);
}

Result<int64_t> NumericDocValuesUpdate::value() const {
    if (!value_.has_value()) {
        return err(LuceneError::illegal_state("getValue requires an update value"));
    }
    return value_.value();
}

std::string NumericDocValuesUpdate::value_to_string() const {
    return value_.has_value() ? std::to_string(value_.value()) : "null";
}

// ---- BinaryDocValuesUpdate ----

BinaryDocValuesUpdate BinaryDocValuesUpdate::of(BytesRef value) {
    return BinaryDocValuesUpdate(std::move(value));
}

BinaryDocValuesUpdate BinaryDocValuesUpdate::no_value() {
    return BinaryDocValuesUpdate(std::nullopt);
}

Result<BytesRef> BinaryDocValuesUpdate::value() const {
    if (!value_.has_value()) {
        return err(LuceneError::illegal_state("getValue requires an update value"));
    }
    return value_.value();
}

std::string BinaryDocValuesUpdate::value_to_string() const {
    return value_.has_value() ? value_.value().to_display_string() : "null";
}

// ---- DocValuesUpdate ----

DocValuesUpdate DocValuesUpdate::create(DocValuesType doc_values_type, Term term, std::string field,
                                        int32_t doc_id_upto, DocValuesUpdateValue value) {
    return DocValuesUpdate(doc_values_type, std::move(term), std::move(field), doc_id_upto, std::move(value));
}

DocValuesUpdate DocValuesUpdate::with_optional_term(DocValuesType doc_values_type, std::optional<Term> term,
                                                   std::string field, int32_t doc_id_upto,
                                                   DocValuesUpdateValue value) {
    return DocValuesUpdate(doc_values_type, std::move(term), std::move(field), doc_id_upto, std::move(value));
}

Result<const Term *> DocValuesUpdate::buffered_term() const {
    if (!term_.has_value()) {
        return err(LuceneError::illegal_state("buffered DocValues update requires a term"));
    }
    return &term_.value();
}

bool DocValuesUpdate::has_value() const {
    return std::visit([](const auto &update) { return update.has_value(); }, sub_update_);
}

Result<const NumericDocValuesUpdate *> DocValuesUpdate::numeric_update() const {
    const auto *update = std::get_if<NumericDocValuesUpdate>(&sub_update_);
    if (update == nullptr) {
        return err(LuceneError::illegal_state("the doc values update is not numeric"));
    }
    return update;
}

Result<const BinaryDocValuesUpdate *> DocValuesUpdate::binary_update() const {
    const auto *update = std::get_if<BinaryDocValuesUpdate>(&sub_update_);
    if (update == nullptr) {
        return err(LuceneError::illegal_state("the doc values update is not binary"));
    }
    return update;
}

std::string DocValuesUpdate::to_string() const {
    std::string text = "term=";
    text += term_.has_value() ? term_.value().to_string() : "null";
    text += ", field=" + field_ + ", value=";
    text += std::visit([](const auto &update) { return update.value_to_string(); }, sub_update_);
    text += ", docIDUpTo=" + std::to_string(doc_id_upto_);
    return text;
}

} // namespace lucene
