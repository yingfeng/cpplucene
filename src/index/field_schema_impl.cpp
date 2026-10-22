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

module lucene_core:field_schema.impl;

import :field_schema;

import :error;
import :field_infos;
import :index_types;
import :types;

import std;

namespace lucene {

namespace {

constexpr std::string_view ERR_MSG = "Inconsistency of field data structures across documents for field ";

[[nodiscard]] std::string_view enum_name(IndexOptions value) {
    switch (value) {
        case IndexOptions::None:
            return "NONE";
        case IndexOptions::Docs:
            return "DOCS";
        case IndexOptions::DocsAndFreqs:
            return "DOCS_AND_FREQS";
        case IndexOptions::DocsAndFreqsAndPositions:
            return "DOCS_AND_FREQS_AND_POSITIONS";
        case IndexOptions::DocsAndFreqsAndPositionsAndOffsets:
            return "DOCS_AND_FREQS_AND_POSITIONS_AND_OFFSETS";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::string_view enum_name(DocValuesType value) {
    switch (value) {
        case DocValuesType::None:
            return "NONE";
        case DocValuesType::Numeric:
            return "NUMERIC";
        case DocValuesType::Binary:
            return "BINARY";
        case DocValuesType::Sorted:
            return "SORTED";
        case DocValuesType::SortedNumeric:
            return "SORTED_NUMERIC";
        case DocValuesType::SortedSet:
            return "SORTED_SET";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::string_view enum_name(DocValuesSkipIndexType value) {
    switch (value) {
        case DocValuesSkipIndexType::None:
            return "NONE";
        case DocValuesSkipIndexType::Range:
            return "RANGE";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::string_view enum_name(VectorEncoding value) {
    switch (value) {
        case VectorEncoding::Byte:
            return "BYTE";
        case VectorEncoding::Float32:
            return "FLOAT32";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::string_view enum_name(VectorSimilarityFunction value) {
    switch (value) {
        case VectorSimilarityFunction::Euclidean:
            return "EUCLIDEAN";
        case VectorSimilarityFunction::DotProduct:
            return "DOT_PRODUCT";
        case VectorSimilarityFunction::Cosine:
            return "COSINE";
        case VectorSimilarityFunction::MaximumInnerProduct:
            return "MAXIMUM_INNER_PRODUCT";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::string_view bool_name(bool value) { return value ? "true" : "false"; }

} // namespace

FieldSchema::FieldSchema(std::string name) : name_(std::move(name)) {}

void FieldSchema::reset(int32_t doc) {
    doc_id_ = doc;
    omit_norms_ = false;
    store_term_vector_ = false;
    index_options_ = IndexOptions::None;
    doc_values_type_ = DocValuesType::None;
    doc_values_skip_index_ = DocValuesSkipIndexType::None;
    point_dimension_count_ = 0;
    point_index_dimension_count_ = 0;
    point_num_bytes_ = 0;
    vector_dimension_ = 0;
    vector_encoding_ = VectorEncoding::Float32;
    vector_similarity_function_ = VectorSimilarityFunction::Euclidean;
}

VoidResult FieldSchema::assert_same(std::string_view label, std::string_view expected,
                                   std::string_view given) const {
    if (expected != given) {
        return err(LuceneError::illegal_argument(std::string(ERR_MSG) + "[" + name_ + "] of doc [" +
                                                 std::to_string(doc_id_) + "]. " + std::string(label) +
                                                 ": expected '" + std::string(expected) + "', but it has '" +
                                                 std::string(given) + "'."));
    }
    return {};
}

VoidResult FieldSchema::update_attributes(
        const std::map<std::string, std::string, std::less<>> &attrs) {
    for (const auto &[key, value] : attrs) {
        attributes_.insert_or_assign(key, value);
    }
    return {};
}

VoidResult FieldSchema::set_index_options(IndexOptions index_options, bool omit_norms,
                                          bool store_term_vector) {
    if (index_options_ == IndexOptions::None) {
        index_options_ = index_options;
        omit_norms_ = omit_norms;
        store_term_vector_ = store_term_vector;
        return {};
    }
    if (auto same = assert_same("index options", enum_name(index_options_), enum_name(index_options));
        !same.has_value()) {
        return same;
    }
    if (auto same = assert_same("omit norms", bool_name(omit_norms_), bool_name(omit_norms)); !same.has_value()) {
        return same;
    }
    return assert_same("store term vector", bool_name(store_term_vector_), bool_name(store_term_vector));
}

VoidResult FieldSchema::set_doc_values(DocValuesType doc_values_type,
                                       DocValuesSkipIndexType doc_values_skip_index) {
    if (doc_values_type_ == DocValuesType::None) {
        doc_values_type_ = doc_values_type;
        doc_values_skip_index_ = doc_values_skip_index;
        return {};
    }
    if (auto same = assert_same("doc values type", enum_name(doc_values_type_), enum_name(doc_values_type));
        !same.has_value()) {
        return same;
    }
    return assert_same("doc values skip index type", enum_name(doc_values_skip_index_),
                       enum_name(doc_values_skip_index));
}

VoidResult FieldSchema::set_points(size_t dimension_count, size_t index_dimension_count, size_t num_bytes) {
    if (point_index_dimension_count_ == 0) {
        point_dimension_count_ = dimension_count;
        point_index_dimension_count_ = index_dimension_count;
        point_num_bytes_ = num_bytes;
        return {};
    }
    if (auto same = assert_same("point dimension", std::to_string(point_dimension_count_),
                                std::to_string(dimension_count));
        !same.has_value()) {
        return same;
    }
    if (auto same = assert_same("point index dimension", std::to_string(point_index_dimension_count_),
                                std::to_string(index_dimension_count));
        !same.has_value()) {
        return same;
    }
    return assert_same("point num bytes", std::to_string(point_num_bytes_), std::to_string(num_bytes));
}

VoidResult FieldSchema::set_vectors(VectorEncoding encoding, VectorSimilarityFunction similarity_function,
                                   size_t dimension) {
    if (vector_dimension_ == 0) {
        vector_encoding_ = encoding;
        vector_similarity_function_ = similarity_function;
        vector_dimension_ = dimension;
        return {};
    }
    if (auto same = assert_same("vector encoding", enum_name(vector_encoding_), enum_name(encoding));
        !same.has_value()) {
        return same;
    }
    if (auto same = assert_same("vector similarity function", enum_name(vector_similarity_function_),
                                enum_name(similarity_function));
        !same.has_value()) {
        return same;
    }
    return assert_same("vector dimension", std::to_string(vector_dimension_), std::to_string(dimension));
}

VoidResult FieldSchema::assert_same_schema(const FieldInfo &field_info) const {
    if (auto same = assert_same("index options", enum_name(field_info.index_options), enum_name(index_options_));
        !same.has_value()) {
        return same;
    }
    if (auto same = assert_same("omit norms", bool_name(field_info.omit_norms), bool_name(omit_norms_));
        !same.has_value()) {
        return same;
    }
    if (auto same = assert_same("store term vector", bool_name(field_info.has_term_vectors()),
                                bool_name(store_term_vector_));
        !same.has_value()) {
        return same;
    }
    if (auto same = assert_same("doc values type", enum_name(field_info.doc_values_type),
                                enum_name(doc_values_type_));
        !same.has_value()) {
        return same;
    }
    if (auto same = assert_same("doc values skip index type", enum_name(field_info.doc_values_skip_index),
                                enum_name(doc_values_skip_index_));
        !same.has_value()) {
        return same;
    }
    if (auto same = assert_same("vector similarity function", enum_name(field_info.vector_similarity),
                                enum_name(vector_similarity_function_));
        !same.has_value()) {
        return same;
    }
    if (auto same = assert_same("vector encoding", enum_name(field_info.vector_encoding),
                                enum_name(vector_encoding_));
        !same.has_value()) {
        return same;
    }
    if (auto same = assert_same("vector dimension", std::to_string(field_info.vector_dimension),
                                std::to_string(vector_dimension_));
        !same.has_value()) {
        return same;
    }
    if (auto same = assert_same("point dimension", std::to_string(field_info.point_dimension_count),
                                std::to_string(point_dimension_count_));
        !same.has_value()) {
        return same;
    }
    if (auto same = assert_same("point index dimension", std::to_string(field_info.point_index_dimension_count),
                                std::to_string(point_index_dimension_count_));
        !same.has_value()) {
        return same;
    }
    return assert_same("point num bytes", std::to_string(field_info.point_num_bytes),
                       std::to_string(point_num_bytes_));
}

} // namespace lucene
