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

export module lucene_core:codec_reader;

import :types;
import std;

import :binary_doc_values;
import :doc_values_producer;
import :doc_values_skipper;
import :error;
import :field_infos;
import :fields_producer;
import :index_types;
import :leaf_reader;
import :norms_producer;
import :numeric_doc_values;
import :point_values;
import :points_reader;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :stored_fields;
import :stored_fields_reader;
import :term_vectors;
import :term_vectors_reader;
import :terms;

namespace lucene {

/// A [`LeafReader`] built from the readers of one segment's codec formats.
///
/// Port of the `CodecReader` trait (`core/index/codec_reader.rs`).
///
/// The codec layer gives one reader per file family (stored fields, term
/// vectors, norms, doc values, postings, points); this class is what turns them
/// into the uniform [`LeafReader`] surface the search side expects, including the
/// `DocValuesType` / `omit_norms` validation that rlucene performs on the way
/// through.
///
/// **Deviations** (C++ mechanics only):
///  * the associated reader types collapse into the concrete type-erased
///    interfaces, returned as borrowed pointers (`nullptr` = "this segment has
///    none"), because the segment owns them;
///  * `RawStoredFieldsReader` / `RawTermVectors` are not ported (merge only), and
///    neither are the vector readers;
///  * the cache-helper plumbing (`get_core_cache_helper`) is not ported.
export class CodecReader : public LeafReader {
public:
    ~CodecReader() override = default;

    /// The stored fields reader of the segment, or null.
    [[nodiscard]] virtual StoredFieldsReader *get_fields_reader() = 0;
    /// The term vectors reader of the segment, or null.
    [[nodiscard]] virtual TermVectorsReader *get_term_vectors_reader() = 0;
    /// The norms producer of the segment, or null.
    [[nodiscard]] virtual NormsProducer *get_norms_reader() = 0;
    /// The doc values producer of the segment, or null.
    [[nodiscard]] virtual DocValuesProducer *get_doc_values_reader() = 0;
    /// The postings reader of the segment, or null.
    [[nodiscard]] virtual FieldsProducer *get_postings_reader() = 0;
    /// The points reader of the segment, or null.
    [[nodiscard]] virtual PointsReader *get_points_reader() = 0;

    // ---- LeafReader, implemented on top of the readers above ----

    /// A cursor over a clone of the stored fields reader, with the document id
    /// range checked here rather than trusting the codec.  Fails with
    /// `illegal_state` when the segment has no stored fields reader.
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override;

    /// A cursor over a clone of the term vectors reader, or an empty cursor
    /// (which has no vectors for any document) when the segment has none.
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override;

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) override;

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(std::string_view field) override;
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view field) override;
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view field) override;
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric_doc_values(
            std::string_view field) override;
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set_doc_values(
            std::string_view field) override;
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(std::string_view field) override;
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view field) override;
    [[nodiscard]] Result<std::shared_ptr<PointValues>> point_values(std::string_view field) override;

    /// Checks the integrity of every reader this segment holds.
    ///
    /// Port of rlucene's `default_check_integrity`: it visits postings, norms,
    /// doc values, stored fields, term vectors and points.  This may read whole
    /// files, so it is for `CheckIndex` and tests only.
    [[nodiscard]] virtual VoidResult check_integrity();

protected:
    /// The [`FieldInfo`] of `field` when the field exists **and** its doc values
    /// type is `expected`; null otherwise.
    ///
    /// Null also covers the two cases rlucene folds into `Ok(None)`: the field
    /// does not exist, or it was indexed without doc values of that type.
    [[nodiscard]] Result<const FieldInfo *> doc_values_field(std::string_view field, DocValuesType expected) const;
};

} // namespace lucene
