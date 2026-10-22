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

export module lucene_core:field_type;

import :types;
import :error;
import :index_types;

import std;

namespace lucene {

/// Describes the properties of a field.  Port of `document/field_type.rs`.
///
/// The type is mutable until `freeze()` is called; every setter checks the
/// frozen flag first, exactly like rlucene.
export class FieldType {
public:
    FieldType() = default;

    /// A mutable copy of another type's properties.
    [[nodiscard]] static FieldType from(const FieldType &other);

    // ---- getters ----
    [[nodiscard]] bool stored() const noexcept { return stored_; }
    [[nodiscard]] bool tokenized() const noexcept { return tokenized_; }
    [[nodiscard]] bool store_term_vectors() const noexcept { return store_term_vectors_; }
    [[nodiscard]] bool store_term_vector_offsets() const noexcept { return store_term_vector_offsets_; }
    [[nodiscard]] bool store_term_vector_positions() const noexcept { return store_term_vector_positions_; }
    [[nodiscard]] bool store_term_vector_payloads() const noexcept { return store_term_vector_payloads_; }
    [[nodiscard]] bool omit_norms() const noexcept { return omit_norms_; }
    [[nodiscard]] IndexOptions index_options() const noexcept { return index_options_; }
    [[nodiscard]] DocValuesType doc_values_type() const noexcept { return doc_values_type_; }
    [[nodiscard]] DocValuesSkipIndexType doc_values_skip_index_type() const noexcept {
        return doc_values_skip_index_;
    }
    /// `true` when `index_options() != IndexOptions::None`.
    [[nodiscard]] bool is_indexed() const noexcept { return index_options_ != IndexOptions::None; }
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

    [[nodiscard]] size_t point_dimension_count() const noexcept { return dimension_count_; }
    [[nodiscard]] size_t point_index_dimension_count() const noexcept { return index_dimension_count_; }
    [[nodiscard]] size_t point_num_bytes() const noexcept { return dimension_num_bytes_; }

    [[nodiscard]] int32_t vector_dimension() const noexcept { return vector_dimension_; }
    [[nodiscard]] VectorEncoding vector_encoding() const noexcept { return vector_encoding_; }
    [[nodiscard]] VectorSimilarityFunction vector_similarity_function() const noexcept {
        return vector_similarity_function_;
    }

    [[nodiscard]] const std::map<std::string, std::string, std::less<>> &attributes() const noexcept {
        return attributes_;
    }

    // ---- setters ----
    [[nodiscard]] VoidResult set_stored(bool value);
    [[nodiscard]] VoidResult set_tokenized(bool value);
    [[nodiscard]] VoidResult set_store_term_vectors(bool value);
    [[nodiscard]] VoidResult set_store_term_vector_offsets(bool value);
    [[nodiscard]] VoidResult set_store_term_vector_positions(bool value);
    [[nodiscard]] VoidResult set_store_term_vector_payloads(bool value);
    [[nodiscard]] VoidResult set_omit_norms(bool value);
    [[nodiscard]] VoidResult set_index_options(IndexOptions value);
    [[nodiscard]] VoidResult set_doc_values_type(DocValuesType value);
    [[nodiscard]] VoidResult set_doc_values_skip_index_type(DocValuesSkipIndexType value);

    /// Sets the point dimensions, using `dimension_count` as the index
    /// dimension count (`set_dimensions` in rlucene).
    [[nodiscard]] VoidResult set_dimensions(size_t dimension_count, size_t dimension_num_bytes);

    /// Sets the point dimensions; all three must be `0`, or `count/index_count`
    /// must be in `1..=8` with `num_bytes` in `1..=8`.
    [[nodiscard]] VoidResult set_dimensions(size_t dimension_count,
                                            size_t index_dimension_count,
                                            size_t dimension_num_bytes);

    /// Sets the vector attributes of this field type.
    [[nodiscard]] VoidResult set_vector_attributes(int32_t dimension,
                                                   VectorEncoding encoding,
                                                   VectorSimilarityFunction similarity);

    /// Stores a free-form attribute, replacing an existing one.
    [[nodiscard]] Result<std::optional<std::string>> put_attribute(std::string key, std::string value);

    /// Prevents future mutations.
    void freeze() noexcept { frozen_ = true; }

    /// Returns an error when this type is frozen.
    [[nodiscard]] VoidResult check_if_frozen() const;

    /// Throws (`illegal_state`) when the combination of properties is invalid.
    [[nodiscard]] VoidResult validate() const;

    /// Human readable rendering, identical to rlucene's `Display`.
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool operator==(const FieldType &other) const;

private:
    bool stored_{false};
    bool tokenized_{true};
    bool store_term_vectors_{false};
    bool store_term_vector_offsets_{false};
    bool store_term_vector_positions_{false};
    bool store_term_vector_payloads_{false};
    bool omit_norms_{false};
    IndexOptions index_options_{IndexOptions::None};
    bool frozen_{false};
    DocValuesType doc_values_type_{DocValuesType::None};
    DocValuesSkipIndexType doc_values_skip_index_{DocValuesSkipIndexType::None};
    size_t dimension_count_{0};
    size_t index_dimension_count_{0};
    size_t dimension_num_bytes_{0};
    int32_t vector_dimension_{0};
    VectorEncoding vector_encoding_{VectorEncoding::Float32};
    VectorSimilarityFunction vector_similarity_function_{VectorSimilarityFunction::Euclidean};
    std::map<std::string, std::string, std::less<>> attributes_{};
};

} // namespace lucene
