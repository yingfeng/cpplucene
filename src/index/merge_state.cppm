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

export module lucene_core:merge_state;

import :types;
import std;

import :bits;
import :codec_reader;
import :doc_map;
import :doc_values_producer;
import :error;
import :field_infos;
import :fields_producer;
import :info_stream;
import :norms_producer;
import :packed_long_values;
import :points_reader;
import :segment_info;
import :stored_fields_reader;
import :term_vectors_reader;

namespace lucene {

/// Wraps a borrowed pointer into a `shared_ptr` that does not own it.
///
/// [`MergeState`] keeps "the segment reader itself" and "a merge instance I must
/// close" in the same slot; the reference implementation spells that `Option<Enum<A, A::MergeInstance>>`
/// while this port uses one `shared_ptr` per slot, and this helper builds the
/// non-owning variant.
export template <class T>
[[nodiscard]] std::shared_ptr<T> merge_state_borrow(T *pointer) {
    return std::shared_ptr<T>(pointer, [](T *) {});
}

/// A [`DocMap`] that only remaps around deletions.
///
/// Port of the reference implementation's `MergeStateDocMapImpl` restricted to its `Deletions` hook:
/// the index-sort flavour (`Sorted`, which additionally reorders documents) is
/// not ported because this port has no index sorting.
export class MergeStateDocMap final : public DocMap {
public:
    /// `live_docs` is null when the segment has no deletions; `del_doc_map` is
    /// then null as well.
    [[nodiscard]] static std::shared_ptr<MergeStateDocMap> new_deletions(
            std::shared_ptr<const Bits> live_docs, std::shared_ptr<const PackedLongValues> del_doc_map,
            int32_t doc_base);

    /// The mapped doc id, or `-1` for a deleted document.
    [[nodiscard]] Result<int32_t> get(int32_t doc_id) const override;

private:
    MergeStateDocMap(std::shared_ptr<const Bits> live_docs, std::shared_ptr<const PackedLongValues> del_doc_map,
                     int32_t doc_base);

    std::shared_ptr<const Bits> live_docs_;
    std::shared_ptr<const PackedLongValues> del_doc_map_;
    int32_t doc_base_;
};

/// Builds the "doc id minus the number of deleted documents before it" map.
///
/// Port of `remove_deletes`: entry `i` holds the new doc id of document `i`
/// ignoring the deletions that follow it, so `del_doc_map->get(i)` is meaningful
/// only for live documents.
[[nodiscard]] Result<std::shared_ptr<const PackedLongValues>> remove_deletes(int32_t max_doc, const Bits &live_docs);

/// The subset of a [`MergeState`] that per-field codecs are allowed to see.
///
/// Port of the reference implementation's `MergeStateMeta` / `MergeStateAccess` pair.  The reference implementation needs
/// the trait to keep the producer type statically dispatched; here every codec
/// reader is already type erased, so [`MergeState`] itself is the accessor and
/// this struct only lets the postings merge share one cheap snapshot.
export struct MergeStateMeta {
    size_t fields_producers_len{0};
    std::shared_ptr<const std::vector<std::shared_ptr<MergeStateDocMap>>> doc_maps;
    bool needs_index_sort{false};
    std::shared_ptr<const FieldInfos> merge_field_infos;
    std::shared_ptr<const std::vector<std::shared_ptr<const FieldInfos>>> field_infos;
};

/// Common state shared by the codecs that take part in a segment merge.
///
/// Port of `MergeState` (`core/index/merge_state.rs`).
///
/// For every input segment it holds the codec readers to merge from (a merge
/// instance when the codec has one), the live-docs bits, the field infos and the
/// [`MergeStateDocMap`] that rebases and filters document ids.
///
/// **Deviations** (C++ mechanics and scope):
///  * built through [`create`] (a constructor cannot report `LuceneError`), and
///    handed out as a `unique_ptr`, which is why `segment_info` stays a
///    reference the caller must keep alive;
///  * no index sorting: `needs_index_sort` is always `false` and the doc maps are
///    built from deletions alone (`MergeStateDocMap::Sorted` is not ported);
///  * no intra-merge `TaskExecutor` (the port merges single threaded);
///  * readers are held in `shared_ptr`s that may be non-owning aliases of the
///    segment reader, so a merge instance is closed when the merge state dies;
///  * the vector (knn) readers are not ported.
export class MergeState {
public:
    using DocMaps = std::vector<std::shared_ptr<MergeStateDocMap>>;
    using FieldInfosList = std::vector<std::shared_ptr<const FieldInfos>>;

    /// Builds the merge state for `readers`, which must stay alive for as long
    /// as the returned object is used, and sets `segment_info.max_doc` to the
    /// number of live documents.
    [[nodiscard]] static Result<std::unique_ptr<MergeState>> create(std::span<CodecReader *const> readers,
                                                                   SegmentInfo &segment_info,
                                                                   std::shared_ptr<InfoStreamEnum> info_stream);

    /// Defined in the implementation unit on purpose: the member list holds
    /// `std::shared_ptr` specialisations that `import std` instantiated inside
    /// the module, so an implicitly generated destructor would re-instantiate
    /// them in every importing translation unit.
    ~MergeState();

    MergeState(const MergeState &) = delete;
    MergeState &operator=(const MergeState &) = delete;
    MergeState(MergeState &&) = delete;
    MergeState &operator=(MergeState &&) = delete;

    /// `SegmentInfo` of the newly merged segment.
    SegmentInfo &segment_info;
    /// Maps document ids from the old segments to the new segment.
    std::shared_ptr<const DocMaps> doc_maps;
    /// Field infos of the newly merged segment (filled in by `SegmentMerger`).
    std::shared_ptr<const FieldInfos> merge_field_infos;
    /// Stored fields producers being merged (null entries are allowed).
    std::vector<std::shared_ptr<StoredFieldsReader>> stored_fields_readers;
    /// Term vector producers being merged.
    std::vector<std::shared_ptr<TermVectorsReader>> term_vectors_readers;
    /// Norms producers being merged.
    std::vector<std::shared_ptr<NormsProducer>> norms_producers;
    /// Doc values producers being merged.
    std::vector<std::shared_ptr<DocValuesProducer>> doc_values_producers;
    /// Postings being merged.
    std::vector<std::shared_ptr<FieldsProducer>> fields_producers;
    /// Point readers being merged.
    std::vector<std::shared_ptr<PointsReader>> points_readers;
    /// Field infos of the segments being merged.
    std::shared_ptr<const FieldInfosList> field_infos;
    /// Live docs of each segment, null when it has no deletions.
    std::vector<std::shared_ptr<const Bits>> live_docs;
    /// `true` when the merged segment has to be re-sorted (never, in this port).
    bool needs_index_sort{false};
    /// `max_doc` of each input segment.
    std::vector<int32_t> max_docs;
    /// Debug stream.
    std::shared_ptr<InfoStreamEnum> info_stream;

    /// A cheap snapshot for code that only needs the doc maps and field infos.
    [[nodiscard]] MergeStateMeta get_meta() const;

    /// Number of segments being merged.
    [[nodiscard]] size_t num_readers() const noexcept { return max_docs.size(); }

private:
    explicit MergeState(SegmentInfo &segment_info, std::shared_ptr<InfoStreamEnum> info_stream);
};

} // namespace lucene
