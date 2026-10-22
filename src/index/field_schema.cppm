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

export module lucene_core:field_schema;

import :types;
import std;

import :error;
import :field_infos;
import :index_types;

namespace lucene {

/// The schema of one field, as seen by the current document.
///
/// Port of `FieldSchema` (`indexing_chain.rs`).  Every new document resets it;
/// while the document's fields are processed it accumulates the options they
/// carry.  At the end of the document it is compared with the [`FieldInfo`]
/// (built from the first document of the segment that had the field), so an
/// inconsistent field cannot silently corrupt the segment.
///
/// The reference implementation declares the class at the bottom of `indexing_chain.rs`; C++ puts it
/// in its own partition because the indexing chain imports it and the class is
/// big enough to test on its own.
export class FieldSchema {
public:
    explicit FieldSchema(std::string name);

    FieldSchema(const FieldSchema &) = default;
    FieldSchema &operator=(const FieldSchema &) = default;
    FieldSchema(FieldSchema &&) noexcept = default;
    FieldSchema &operator=(FieldSchema &&) noexcept = default;
    ~FieldSchema() = default;

    [[nodiscard]] const std::string &name() const noexcept { return name_; }
    [[nodiscard]] int32_t doc_id() const noexcept { return doc_id_; }
    [[nodiscard]] IndexOptions index_options() const noexcept { return index_options_; }
    [[nodiscard]] bool omit_norms() const noexcept { return omit_norms_; }
    [[nodiscard]] bool store_term_vector() const noexcept { return store_term_vector_; }
    [[nodiscard]] DocValuesType doc_values_type() const noexcept { return doc_values_type_; }
    [[nodiscard]] DocValuesSkipIndexType doc_values_skip_index() const noexcept {
        return doc_values_skip_index_;
    }
    [[nodiscard]] size_t point_dimension_count() const noexcept { return point_dimension_count_; }
    [[nodiscard]] size_t point_index_dimension_count() const noexcept { return point_index_dimension_count_; }
    [[nodiscard]] size_t point_num_bytes() const noexcept { return point_num_bytes_; }
    [[nodiscard]] size_t vector_dimension() const noexcept { return vector_dimension_; }
    [[nodiscard]] VectorEncoding vector_encoding() const noexcept { return vector_encoding_; }
    [[nodiscard]] VectorSimilarityFunction vector_similarity_function() const noexcept {
        return vector_similarity_function_;
    }
    [[nodiscard]] const std::map<std::string, std::string, std::less<>> &attributes() const noexcept {
        return attributes_;
    }

    /// Re-initializes the schema for the document `doc`.
    void reset(int32_t doc);

    /// Merges `attrs` into the attributes seen so far.
    [[nodiscard]] VoidResult update_attributes(const std::map<std::string, std::string, std::less<>> &attrs);

    /// Records the index options of a field instance; the first instance of the
    /// document wins and later ones must agree.
    [[nodiscard]] VoidResult set_index_options(IndexOptions index_options, bool omit_norms,
                                               bool store_term_vector);

    /// Records the doc values type of a field instance (same rule as
    /// [`set_index_options`]).
    [[nodiscard]] VoidResult set_doc_values(DocValuesType doc_values_type,
                                            DocValuesSkipIndexType doc_values_skip_index);

    /// Records the point dimensions of a field instance.
    [[nodiscard]] VoidResult set_points(size_t dimension_count, size_t index_dimension_count, size_t num_bytes);

    /// Records the vector attributes of a field instance.
    [[nodiscard]] VoidResult set_vectors(VectorEncoding encoding,
                                        VectorSimilarityFunction similarity_function, size_t dimension);

    /// Verifies that the schema of this document matches `field_info`.
    [[nodiscard]] VoidResult assert_same_schema(const FieldInfo &field_info) const;

private:
    [[nodiscard]] VoidResult assert_same(std::string_view label, std::string_view expected,
                                         std::string_view given) const;

    std::string name_;
    int32_t doc_id_{0};
    std::map<std::string, std::string, std::less<>> attributes_;
    bool omit_norms_{false};
    bool store_term_vector_{false};
    IndexOptions index_options_{IndexOptions::None};
    DocValuesType doc_values_type_{DocValuesType::None};
    DocValuesSkipIndexType doc_values_skip_index_{DocValuesSkipIndexType::None};
    size_t point_dimension_count_{0};
    size_t point_index_dimension_count_{0};
    size_t point_num_bytes_{0};
    size_t vector_dimension_{0};
    VectorEncoding vector_encoding_{VectorEncoding::Float32};
    VectorSimilarityFunction vector_similarity_function_{VectorSimilarityFunction::Euclidean};
};

} // namespace lucene
