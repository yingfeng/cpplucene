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

/// Leaf metadata types shared by `document` and `index`.
///
/// In rlucene these live under `core/index` (`index_options.rs`,
/// `doc_values_type.rs`, ...).  They are collected in one dependency-free
/// partition here so `FieldType` can use them without a partition cycle
/// between the `document` and `index` subsystems.
export module lucene_core:index_types;

import :types;

import std;

namespace lucene {

/// What is recorded into the inverted index for a field.
///
/// The declaration order matters: **every** `IndexOptions` except `None`
/// implies the previous ones.
export enum class IndexOptions : int32_t {
    /// Not indexed.
    None = 0,
    /// Only documents are indexed: term frequencies and positions are omitted.
    Docs = 1,
    /// Documents and term frequencies: normal scoring, no positions.
    DocsAndFreqs = 2,
    /// Documents, frequencies and positions (the full-text default).
    DocsAndFreqsAndPositions = 3,
    /// Documents, frequencies, positions and character offsets.
    DocsAndFreqsAndPositionsAndOffsets = 4,
};

/// The type of per-document values stored for a field.
export enum class DocValuesType : int32_t {
    None = 0,
    /// A per-document number.
    Numeric = 1,
    /// Per-document bytes.
    Binary = 2,
    /// Pre-sorted bytes, shared per document via an ordinal.
    Sorted = 3,
    /// A pre-sorted `i64[]`.
    SortedNumeric = 4,
    /// A pre-sorted set of byte sequences.
    SortedSet = 5,
};

/// Whether a skip index is written alongside doc values.
export enum class DocValuesSkipIndexType : int32_t {
    None = 0,
    /// Records min/max values per range of doc IDs.
    Range = 1,
};

/// How the samples of a vector field are encoded.
export enum class VectorEncoding : int32_t {
    /// 8 bits per sample; values must fit in `[-128, 127]`.
    Byte = 0,
    /// 32 bits per sample, IEEE single precision.
    Float32 = 1,
};

/// Number of bytes per sample for a [`VectorEncoding`].
[[nodiscard]] constexpr size_t vector_encoding_bytes(VectorEncoding encoding) noexcept {
    return encoding == VectorEncoding::Byte ? 1 : 4;
}

/// Distance/similarity function used by vector search.
export enum class VectorSimilarityFunction : int32_t {
    /// Euclidean distance.
    Euclidean = 0,
    /// Dot product; requires normalized vectors.
    DotProduct = 1,
    /// Cosine similarity.
    Cosine = 2,
    /// Maximum inner product.
    MaximumInnerProduct = 3,
};

} // namespace lucene
