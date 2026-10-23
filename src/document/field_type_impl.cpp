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

module lucene_core:field_type.impl;

import :field_type;
import :types;
import :error;
import :index_types;

import std;

namespace lucene {

namespace {
// `core/index/point_values.rs`
constexpr size_t MAX_DIMENSIONS = 16;
constexpr size_t MAX_INDEX_DIMENSIONS = 8;
// Sixteen bytes per dimension is what the point index supports: an `InetAddressPoint`
// is a single 128-bit dimension.
constexpr size_t MAX_NUM_BYTES = 16;
} // namespace

FieldType FieldType::from(const FieldType &other) {
    FieldType copy = other;
    copy.frozen_ = false;
    return copy;
}

VoidResult FieldType::check_if_frozen() const {
    if (frozen_) {
        return err(LuceneError::illegal_state("this FieldType is already frozen and cannot be changed"));
    }
    return {};
}

VoidResult FieldType::set_stored(bool value) {
    if (auto r = check_if_frozen(); !r) {
        return r;
    }
    stored_ = value;
    return {};
}

VoidResult FieldType::set_tokenized(bool value) {
    if (auto r = check_if_frozen(); !r) {
        return r;
    }
    tokenized_ = value;
    return {};
}

VoidResult FieldType::set_store_term_vectors(bool value) {
    if (auto r = check_if_frozen(); !r) {
        return r;
    }
    store_term_vectors_ = value;
    return {};
}

VoidResult FieldType::set_store_term_vector_offsets(bool value) {
    if (auto r = check_if_frozen(); !r) {
        return r;
    }
    store_term_vector_offsets_ = value;
    return {};
}

VoidResult FieldType::set_store_term_vector_positions(bool value) {
    if (auto r = check_if_frozen(); !r) {
        return r;
    }
    store_term_vector_positions_ = value;
    return {};
}

VoidResult FieldType::set_store_term_vector_payloads(bool value) {
    if (auto r = check_if_frozen(); !r) {
        return r;
    }
    store_term_vector_payloads_ = value;
    return {};
}

VoidResult FieldType::set_omit_norms(bool value) {
    if (auto r = check_if_frozen(); !r) {
        return r;
    }
    omit_norms_ = value;
    return {};
}

VoidResult FieldType::set_index_options(IndexOptions value) {
    if (auto r = check_if_frozen(); !r) {
        return r;
    }
    index_options_ = value;
    return {};
}

VoidResult FieldType::set_doc_values_type(DocValuesType value) {
    if (auto r = check_if_frozen(); !r) {
        return r;
    }
    doc_values_type_ = value;
    return {};
}

VoidResult FieldType::set_doc_values_skip_index_type(DocValuesSkipIndexType value) {
    if (auto r = check_if_frozen(); !r) {
        return r;
    }
    doc_values_skip_index_ = value;
    return {};
}

VoidResult FieldType::set_dimensions(size_t dimension_count, size_t dimension_num_bytes) {
    return set_dimensions(dimension_count, dimension_count, dimension_num_bytes);
}

VoidResult FieldType::set_dimensions(size_t dimension_count,
                                     size_t index_dimension_count,
                                     size_t dimension_num_bytes) {
    if (auto r = check_if_frozen(); !r) {
        return r;
    }
    if (dimension_count > MAX_DIMENSIONS) {
        return err(LuceneError::illegal_argument(
            std::format("dimensionCount must be <= {}; got {}", MAX_DIMENSIONS, dimension_count)));
    }
    if (index_dimension_count > dimension_count) {
        return err(LuceneError::illegal_argument(
            std::format("indexDimensionCount must be <= dimensionCount: {}; got {}",
                        dimension_count,
                        index_dimension_count)));
    }
    if (index_dimension_count > MAX_INDEX_DIMENSIONS) {
        return err(LuceneError::illegal_argument(
            std::format("indexDimensionCount must be <= {}; got {}", MAX_INDEX_DIMENSIONS, index_dimension_count)));
    }
    if (dimension_num_bytes > MAX_NUM_BYTES) {
        return err(LuceneError::illegal_argument(
            std::format("dimensionNumBytes must be <= {}; got {}", MAX_NUM_BYTES, dimension_num_bytes)));
    }
    if (dimension_count == 0) {
        if (index_dimension_count != 0) {
            return err(LuceneError::illegal_argument(
                std::format("when dimensionCount is 0, indexDimensionCount must be 0; got {}",
                            index_dimension_count)));
        }
        if (dimension_num_bytes != 0) {
            return err(LuceneError::illegal_argument(
                std::format("when dimensionCount is 0, dimensionNumBytes must be 0; got {}", dimension_num_bytes)));
        }
    } else if (index_dimension_count == 0) {
        return err(LuceneError::illegal_argument(
            std::format("when dimensionCount is > 0, indexDimensionCount must be > 0; got {}",
                        index_dimension_count)));
    } else if (dimension_num_bytes == 0) {
        return err(LuceneError::illegal_argument(
            std::format("when dimensionNumBytes is 0, dimensionCount must be 0; got {}", dimension_count)));
    }

    dimension_count_ = dimension_count;
    index_dimension_count_ = index_dimension_count;
    dimension_num_bytes_ = dimension_num_bytes;
    return {};
}

VoidResult FieldType::set_vector_attributes(int32_t dimension,
                                            VectorEncoding encoding,
                                            VectorSimilarityFunction similarity) {
    if (auto r = check_if_frozen(); !r) {
        return r;
    }
    if (dimension <= 0) {
        return err(LuceneError::illegal_argument(std::format("vector numDimensions must be > 0; got {}", dimension)));
    }
    vector_dimension_ = dimension;
    vector_similarity_function_ = similarity;
    vector_encoding_ = encoding;
    return {};
}

Result<std::optional<std::string>> FieldType::put_attribute(std::string key, std::string value) {
    if (auto r = check_if_frozen(); !r) {
        return std::unexpected(std::move(r.error()));
    }
    auto it = attributes_.find(key);
    if (it == attributes_.end()) {
        attributes_.emplace(std::move(key), std::move(value));
        return std::optional<std::string>{};
    }
    std::optional<std::string> previous = std::move(it->second);
    it->second = std::move(value);
    return previous;
}

VoidResult FieldType::validate() const {
    if (dimension_count_ == 0 && dimension_num_bytes_ != 0) {
        return err(LuceneError::illegal_state(
            std::format("point dimension count is 0, but point dimension num bytes is {}", dimension_num_bytes_)));
    }
    if (index_dimension_count_ > dimension_count_) {
        return err(LuceneError::illegal_state(
            std::format("point index dimension count is {}, but point dimension count is {}",
                        index_dimension_count_,
                        dimension_count_)));
    }
    if (dimension_count_ != 0) {
        if (index_dimension_count_ == 0) {
            return err(LuceneError::illegal_state("point index dimension count is 0, but dimension count is nonzero"));
        }
        if (dimension_num_bytes_ == 0) {
            return err(LuceneError::illegal_state("point dimension num bytes is 0, but dimension count is nonzero"));
        }
    }
    if (index_options_ == IndexOptions::None) {
        if (store_term_vectors_) {
            return err(LuceneError::illegal_state(
                "cannot store term vectors for a field that is not indexed"));
        }
        if (store_term_vector_positions_) {
            return err(LuceneError::illegal_state(
                "cannot store term vector positions for a field that is not indexed"));
        }
        if (store_term_vector_offsets_) {
            return err(LuceneError::illegal_state(
                "cannot store term vector offsets for a field that is not indexed"));
        }
        if (store_term_vector_payloads_) {
            return err(LuceneError::illegal_state(
                "cannot store term vector payloads for a field that is not indexed"));
        }
    }
    if (store_term_vector_offsets_ && !store_term_vectors_) {
        return err(LuceneError::illegal_state(
            "cannot store term vector offsets if term vectors are not stored"));
    }
    if (store_term_vector_positions_ && !store_term_vectors_) {
        return err(LuceneError::illegal_state(
            "cannot store term vector positions if term vectors are not stored"));
    }
    if (store_term_vector_payloads_ && !store_term_vectors_) {
        return err(LuceneError::illegal_state(
            "cannot store term vector payloads if term vectors are not stored"));
    }
    if (store_term_vector_offsets_ && index_options_ != IndexOptions::DocsAndFreqsAndPositionsAndOffsets) {
        return err(LuceneError::illegal_state(
            "cannot store term vector offsets unless index options include offsets"));
    }
    if (store_term_vector_positions_ && index_options_ != IndexOptions::DocsAndFreqsAndPositions &&
        index_options_ != IndexOptions::DocsAndFreqsAndPositionsAndOffsets) {
        return err(LuceneError::illegal_state(
            "cannot store term vector positions unless index options include positions"));
    }
    return {};
}

bool FieldType::operator==(const FieldType &other) const {
    return dimension_count_ == other.dimension_count_ && index_dimension_count_ == other.index_dimension_count_ &&
           dimension_num_bytes_ == other.dimension_num_bytes_ && doc_values_type_ == other.doc_values_type_ &&
           doc_values_skip_index_ == other.doc_values_skip_index_ && index_options_ == other.index_options_ &&
           omit_norms_ == other.omit_norms_ &&
           store_term_vector_offsets_ == other.store_term_vector_offsets_ &&
           store_term_vector_payloads_ == other.store_term_vector_payloads_ &&
           store_term_vector_positions_ == other.store_term_vector_positions_ &&
           store_term_vectors_ == other.store_term_vectors_ && stored_ == other.stored_ &&
           tokenized_ == other.tokenized_;
}

std::string FieldType::to_string() const {
    std::string result;
    const auto append = [&result](std::string_view part) {
        if (!result.empty()) {
            result.push_back(',');
        }
        result.append(part);
    };

    if (stored_) {
        result = "stored";
    }
    if (index_options_ != IndexOptions::None) {
        append("indexed");
        if (tokenized_) {
            append("tokenized");
        }
        if (store_term_vectors_) {
            append("termVector");
        }
        if (store_term_vector_offsets_) {
            append("termVectorOffsets");
        }
        if (store_term_vector_positions_) {
            append("termVectorPosition");
        }
        if (store_term_vector_payloads_) {
            append("termVectorPayloads");
        }
        if (omit_norms_) {
            append("omitNorms");
        }
        if (index_options_ != IndexOptions::DocsAndFreqsAndPositions) {
            static constexpr std::string_view kNames[] = {"None",
                                                          "Docs",
                                                          "DocsAndFreqs",
                                                          "DocsAndFreqsAndPositions",
                                                          "DocsAndFreqsAndPositionsAndOffsets"};
            append(std::format("indexOptions={}", kNames[static_cast<size_t>(index_options_)]));
        }
    }
    if (dimension_count_ != 0) {
        append(std::format("pointDimensionCount={},pointIndexDimensionCount={},pointNumBytes={}",
                           dimension_count_,
                           index_dimension_count_,
                           dimension_num_bytes_));
    }
    if (doc_values_type_ != DocValuesType::None) {
        static constexpr std::string_view kNames[] = {"None",
                                                     "Numeric",
                                                     "Binary",
                                                     "Sorted",
                                                     "SortedNumeric",
                                                     "SortedSet"};
        append(std::format("docValuesType={}", kNames[static_cast<size_t>(doc_values_type_)]));
    }
    return result;
}

} // namespace lucene
