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

export module lucene_core:codec;

import :types;
import std;

import :compound_format;
import :doc_values_format;
import :error;
import :field_infos_format;
import :live_docs_format;
import :norms_format;
import :points_format;
import :postings_format;
import :segment_info_format;
import :stored_fields_format;
import :term_vectors_format;

namespace lucene {

/// A set of formats that together read and write one index format generation.
///
/// Port of the `Codec` trait (`codecs/codec.rs`).  rlucene keeps a registry of
/// codecs keyed by name so `segments_N` can pick the right one per segment (see
/// `lucene_core:codec_formats`); what matters here is the dispatch surface: every
/// format hangs off a codec name.
///
/// **Deviations** (C++ mechanics only):
///  * the metadata formats ([`segment_info_format`], [`live_docs_format`],
///    [`field_infos_format`]) are returned by reference: every codec has them;
///  * the data formats are returned by pointer, where a null pointer means "this
///    codec does not provide it".  rlucene returns them by value and therefore
///    cannot express "absent"; the pointer keeps the port honest for a codec that
///    only carries metadata (e.g. [`Lucene99Codec`], which exists to read the
///    tests' `segments_N`) without forcing it to fake data formats it does not
///    have;
///  * `knn_vectors_format` is not part of the interface at all: the HNSW vector
///    formats are out of scope for this port (see the project scope note).
export class Codec {
public:
    virtual ~Codec() = default;

    /// Name recorded in `segments_N` and in every per-segment file header.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// Encodes/decodes terms and postings, or null when the codec has none.
    [[nodiscard]] virtual const PostingsFormat *postings_format() const { return nullptr; }

    /// Encodes/decodes doc values, or null when the codec has none.
    [[nodiscard]] virtual const DocValuesFormat *doc_values_format() const { return nullptr; }

    /// Encodes/decodes stored fields, or null when the codec has none.
    [[nodiscard]] virtual const StoredFieldsFormat *stored_fields_format() const { return nullptr; }

    /// Encodes/decodes term vectors, or null when the codec has none.
    [[nodiscard]] virtual const TermVectorsFormat *term_vectors_format() const { return nullptr; }

    /// Encodes/decodes norms, or null when the codec has none.
    [[nodiscard]] virtual const NormsFormat *norms_format() const { return nullptr; }

    /// Encodes/decodes indexed points, or null when the codec has none.
    [[nodiscard]] virtual const PointsFormat *points_format() const { return nullptr; }

    /// Packs the segment files into a compound file, or null when unsupported.
    [[nodiscard]] virtual const CompoundFormat *compound_format() const { return nullptr; }

    /// Encodes/decodes the field infos (`.fnm`) of a segment.
    [[nodiscard]] virtual const FieldInfosFormat &field_infos_format() const = 0;

    /// Encodes/decodes the segment metadata (`.si`) of a segment.
    [[nodiscard]] virtual const SegmentInfoFormat &segment_info_format() const = 0;

    /// Encodes/decodes the deleted documents (`.liv`) of a segment.
    [[nodiscard]] virtual const LiveDocsFormat &live_docs_format() const = 0;

    /// Called once after construction, so a codec can register its formats.
    virtual void initialize() {}
};

} // namespace lucene
