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

export module lucene_core:lucene101_codec;

import :types;
import std;

import :codec;
import :compound_format;
import :doc_values_format;
import :error;
import :field_infos_format;
import :live_docs_format;
import :lucene101_postings_format;
import :lucene90_compound_format;
import :lucene90_doc_values_format;
import :lucene90_live_docs_format;
import :lucene90_norms_format;
import :lucene90_points_format;
import :lucene90_stored_fields_format;
import :lucene90_term_vectors_format;
import :lucene99_segment_info_format;
import :norms_format;
import :points_format;
import :postings_format;
import :segment_info_format;
import :stored_fields_format;
import :term_vectors_format;

namespace lucene {

/// The default codec of this port: the Lucene 10.1 generation.
///
/// Port of `Lucene101Codec` (`core/codecs/lucene101_codec.rs`).  It owns one
/// instance of every format and hands them out by name.
///
/// **Deviations**:
///  * the reference implementation routes postings and doc values through
///    `PerFieldPostingsFormat` / `PerFieldDocValuesFormat`, whose
///    `get_*_format_for_field` always returns the default format.  Those wrappers
///    also change the per-file segment suffix (`Lucene101_0` style).  This port
///    wires the default formats directly; the per-field wrappers land with the
///    `perfield` package.  Since the port only writes and reads its own segments,
///    the suffix difference is invisible;
///  * the KNN vectors format is absent: HNSW formats are out of scope;
///  * `Mode::BestCompression` fails at `stored_fields_format()` time because the
///    Deflate compressor is not ported (see [`Lucene90StoredFieldsFormat`]).
export class Lucene101Codec final : public Codec {
public:
    /// Stored fields compression mode of the codec.
    enum class Mode : uint8_t {
        /// Trade compression ratio for retrieval speed.
        BestSpeed,
        /// Trade retrieval speed for compression ratio; not ported.
        BestCompression,
    };

    /// Codec name recorded in `segments_N`.
    static constexpr std::string_view CODEC_NAME = "Lucene101";

    /// Creates the codec; fails only when the stored fields mode is unsupported.
    [[nodiscard]] static Result<std::unique_ptr<Lucene101Codec>> create(Mode mode = Mode::BestSpeed);

    [[nodiscard]] std::string_view name() const override { return CODEC_NAME; }

    [[nodiscard]] const PostingsFormat *postings_format() const override { return &postings_format_; }
    [[nodiscard]] const DocValuesFormat *doc_values_format() const override { return &doc_values_format_; }
    [[nodiscard]] const StoredFieldsFormat *stored_fields_format() const override { return &stored_fields_format_; }
    [[nodiscard]] const TermVectorsFormat *term_vectors_format() const override { return &term_vectors_format_; }
    [[nodiscard]] const NormsFormat *norms_format() const override { return &norms_format_; }
    [[nodiscard]] const PointsFormat *points_format() const override { return &points_format_; }
    [[nodiscard]] const CompoundFormat *compound_format() const override { return &compound_format_; }

    [[nodiscard]] const FieldInfosFormat &field_infos_format() const override { return field_infos_format_; }
    [[nodiscard]] const SegmentInfoFormat &segment_info_format() const override { return segment_info_format_; }
    [[nodiscard]] const LiveDocsFormat &live_docs_format() const override { return live_docs_format_; }

    /// The stored fields compression mode this codec writes.
    [[nodiscard]] Mode mode() const noexcept { return mode_; }

private:
    Lucene101Codec(Lucene90StoredFieldsFormat stored_fields, Lucene90TermVectorsFormat term_vectors)
            : stored_fields_format_(std::move(stored_fields)), term_vectors_format_(std::move(term_vectors)) {}

    Lucene101PostingsFormat postings_format_{};
    Lucene90DocValuesFormat doc_values_format_{};
    Lucene90StoredFieldsFormat stored_fields_format_{};
    Lucene90TermVectorsFormat term_vectors_format_;
    Lucene90NormsFormat norms_format_{};
    Lucene90PointsFormatAdapter points_format_{};
    Lucene90CompoundFormat compound_format_{};
    Lucene94FieldInfosFormatAdapter field_infos_format_{};
    Lucene99SegmentInfoFormat segment_info_format_{};
    Lucene90LiveDocsFormat live_docs_format_{};
    Mode mode_{Mode::BestSpeed};
};

} // namespace lucene
