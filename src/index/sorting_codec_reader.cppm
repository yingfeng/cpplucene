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

export module lucene_core:sorting_codec_reader;

import :types;
import std;

import :bits;
import :codec_reader;
import :doc_values_producer;
import :error;
import :field_infos;
import :fields_producer;
import :index_sorter;
import :leaf_reader;
import :norms_producer;
import :point_values;
import :points_reader;
import :sorting_field_readers;
import :sorting_points;
import :sorting_producers;
import :sorting_terms;
import :stored_fields_reader;
import :term_vectors_reader;

namespace lucene {

/// The live documents of a sorted segment.
///
/// Port of `SortingBitsImpl` (`core/index/sorting_codec_reader.rs`).
///
/// Document `N` of the sorted view is document `new_to_old(N)` of the segment it
/// wraps, so a live-documents check has to translate the id before asking.
export class SortingBits final : public Bits {
public:
    /// Wraps `delegate`; `doc_map` and the delegate must outlive the wrapper.
    [[nodiscard]] static std::unique_ptr<SortingBits> create(std::shared_ptr<const Bits> delegate,
                                                            std::shared_ptr<const IndexSortDocMap> doc_map);

    [[nodiscard]] Result<bool> get(size_t index) const override;
    [[nodiscard]] size_t length() const override;
    [[nodiscard]] std::string to_string() const override;

private:
    SortingBits(std::shared_ptr<const Bits> delegate, std::shared_ptr<const IndexSortDocMap> doc_map);

    std::shared_ptr<const Bits> delegate_;
    std::shared_ptr<const IndexSortDocMap> doc_map_;
};

/// A [`CodecReader`] whose readers present the documents in index-sorted order.
///
/// Port of `SortingCodecReader` (`core/index/sorting_codec_reader.rs`).
///
/// A segment that was written before the index sort was applied keeps its
/// documents in insertion order.  This reader wraps every per-file reader of
/// such a segment with the sorting counterparts (stored fields, term vectors,
/// norms, doc values, postings, points), so that reading it through the ordinary
/// [`LeafReader`] surface already yields the sorted document numbers.  It is
/// what lets a sorted segment be flushed: read the unsorted segment through
/// this reader and write the result into a fresh segment.
///
/// **Deviation:** the reader does not take ownership of the wrapped one; it
/// shares it through a `std::shared_ptr`, and the wrappers it hands out keep
/// that owner alive.  Consequently [`close`](:close) is a no-op: the owner of
/// the segment decides when the files are closed.
export class SortingCodecReader final : public CodecReader {
public:
    /// Wraps `delegate`; `doc_map` must describe the sort that produced the new
    /// document order.
    [[nodiscard]] static Result<std::unique_ptr<SortingCodecReader>> create(
            std::shared_ptr<CodecReader> delegate, std::shared_ptr<const IndexSortDocMap> doc_map);

    // ---- CodecReader ----

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override;
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override;
    [[nodiscard]] NormsProducer *get_norms_reader() override;
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override;
    [[nodiscard]] FieldsProducer *get_postings_reader() override;
    [[nodiscard]] PointsReader *get_points_reader() override;

    // ---- LeafReader ----

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override;
    [[nodiscard]] Result<const Bits *> live_docs() const override;

    // ---- IndexReader ----

    [[nodiscard]] Result<int32_t> max_doc() const override;
    [[nodiscard]] Result<int32_t> num_docs() const override;

    // ---- diagnostics ----

    [[nodiscard]] std::string to_string() const override;

    /// The wrapped reader (which still reports the pre-sort document numbers).
    [[nodiscard]] const CodecReader &delegate() const noexcept { return *delegate_; }

private:
    SortingCodecReader(std::shared_ptr<CodecReader> delegate, std::shared_ptr<const IndexSortDocMap> doc_map);

    std::shared_ptr<CodecReader> delegate_;
    std::shared_ptr<const IndexSortDocMap> doc_map_;

    /// The sorting wrappers, built on first use (a segment may have none of a
    /// given family, in which case the corresponding member stays null).
    std::unique_ptr<SortingStoredFieldsReader> stored_fields_;
    std::unique_ptr<SortingTermVectorsReader> term_vectors_;
    std::unique_ptr<SortingNormsProducer> norms_;
    std::unique_ptr<SortingDocValuesProducer> doc_values_;
    std::unique_ptr<SortingFieldsProducer> postings_;
    std::unique_ptr<SortingPointsReader> points_;
    /// Built by `live_docs`, which is const.
    mutable std::unique_ptr<SortingBits> live_docs_;
    mutable bool live_docs_built_{false};
};

} // namespace lucene
