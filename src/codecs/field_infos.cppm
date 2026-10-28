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

export module lucene_core:field_infos;

import :types;
import :error;

import std;

import :data_input;
import :data_output;
import :directory;
import :index_types;
import :io_context;

namespace lucene {

/// Everything the index knows about one field of a segment.
///
/// Port of `FieldInfo` (`index/field_info.rs`) reduced to the data the `.fnm`
/// file stores.  The derived predicates (`has_norms`, `has_offsets`, ...) live
/// here too because both the writer and the readers need them.
export struct FieldInfo {
    /// Dense field number, `0 .. field_count`; the postings and doc values files
    /// are keyed by it.
    int32_t number{0};
    std::string name;

    IndexOptions index_options{IndexOptions::None};
    DocValuesType doc_values_type{DocValuesType::None};
    /// Generation of the doc values file when doc values were updated without
    /// rewriting the segment; `-1` means "the segment generation".
    int64_t doc_values_gen{-1};

    bool store_term_vector{false};
    bool omit_norms{false};
    bool store_payloads{false};
    /// Set for the soft deletes field of the segment.
    bool soft_deletes{false};
    /// Set for the parent field of a block index.
    bool parent{false};

    int32_t point_dimension_count{0};
    int32_t point_index_dimension_count{0};
    int32_t point_num_bytes{0};

    int32_t vector_dimension{0};
    VectorEncoding vector_encoding{VectorEncoding::Float32};
    VectorSimilarityFunction vector_similarity{VectorSimilarityFunction::Euclidean};

    DocValuesSkipIndexType doc_values_skip_index{DocValuesSkipIndexType::None};

    /// Free-form attributes; `Lucene90` stores e.g. `softDeletesField` here.
    std::map<std::string, std::string> attributes;

    [[nodiscard]] bool is_indexed() const noexcept { return index_options != IndexOptions::None; }
    [[nodiscard]] bool has_doc_values() const noexcept { return doc_values_type != DocValuesType::None; }
    [[nodiscard]] bool has_point_values() const noexcept { return point_dimension_count != 0; }
    [[nodiscard]] bool has_vector_values() const noexcept { return vector_dimension != 0; }
    /// Vectors can only be stored with norms when the field is indexed.
    [[nodiscard]] bool has_norms() const noexcept { return is_indexed() && !omit_norms; }
    /// `true` only when the field explicitly stores term vectors.
    ///
    /// Port of `FieldInfo::has_term_vectors` (`field_info.rs`), which is just
    /// `store_term_vector`: a field that merely stores frequencies has no
    /// `.tvd`/`.tvx`/`.tvm` files, so the segment reader must not try to open
    /// them.
    [[nodiscard]] bool has_term_vectors() const noexcept { return is_indexed() && store_term_vector; }
    /// Positions are only available when the field records them.
    [[nodiscard]] bool has_positions() const noexcept {
        return index_options >= IndexOptions::DocsAndFreqsAndPositions;
    }
    [[nodiscard]] bool has_offsets() const noexcept {
        return index_options >= IndexOptions::DocsAndFreqsAndPositionsAndOffsets;
    }
    [[nodiscard]] bool has_payloads() const noexcept { return has_positions() && store_payloads; }
    [[nodiscard]] bool has_freqs() const noexcept { return index_options >= IndexOptions::DocsAndFreqs; }
};

/// All [`FieldInfo`]s of a segment, sorted by field number.
///
/// Port of `FieldInfos` (`index/field_infos.rs`).  This port keeps the list
/// immutable once built: the reference implementation's `FieldInfosBuilder` is what
/// `SegmentWriter` uses, and none of the readers need to mutate it.
export class FieldInfos {
public:
    /// Builds the collection from `fields`, verifying numbering and names.
    ///
    /// Fields are sorted by number, which is the order the `.fnm` file stores
    /// them in and the order the readers rely on.
    [[nodiscard]] static Result<FieldInfos> of(std::vector<FieldInfo> fields);

    /// Empty collection (a segment with no fields).
    [[nodiscard]] static FieldInfos none() { return FieldInfos{}; }

    [[nodiscard]] size_t size() const noexcept { return fields_.size(); }
    [[nodiscard]] bool empty() const noexcept { return fields_.empty(); }

    /// The fields, in number order.
    ///
    /// For a view produced by [`with_filter`] this is only the visible subset,
    /// which is what every writer iterating the collection wants; the
    /// segment-wide predicates (`has_norms`, `has_postings`, ...) follow the same
    /// view.
    [[nodiscard]] const std::vector<FieldInfo> &fields() const noexcept {
        return visible_ ? *visible_ : fields_;
    }

    /// A copy of this collection whose iteration view only reports `names`.
    ///
    /// Port of the reference implementation's `FieldInfosHook::Filter`
    /// (`codecs/perfield/per_field_merge_state.rs`): [`by_name`] and
    /// [`by_number`] keep answering for **every** field, so code that remaps
    /// field numbers across segments still works, but [`fields`] and the
    /// `has_*` predicates report the subset only.  This is what lets a per-field
    /// codec hand one format a merge state that looks like a segment containing
    /// just its own fields.
    [[nodiscard]] FieldInfos with_filter(const std::set<std::string> &names) const {
        FieldInfos copy;
        copy.fields_ = fields_;
        copy.visible_ = std::vector<FieldInfo>{};
        for (const auto &field : fields_) {
            if (names.contains(field.name)) {
                copy.visible_->push_back(field);
            }
        }
        return copy;
    }

    /// Mutable access to a field, for writers that annotate fields while writing.
    ///
    /// The collection is logically immutable once built, but a segment writer
    /// still records attributes during the write (the per-field formats store
    /// their format name and suffix this way) and the `.fnm` file is written
    /// last, so the annotation is visible to it.  The `const_cast` is safe
    /// because nothing else can observe the object between the two points.
    [[nodiscard]] FieldInfo *field_info_mut(std::string_view name) {
        return const_cast<FieldInfo *>(by_name(name));
    }

    /// Field with the given name, or `nullptr`.
    [[nodiscard]] const FieldInfo *by_name(std::string_view name) const;

    /// Field with the given number, or `nullptr`.
    [[nodiscard]] const FieldInfo *by_number(int32_t number) const;

    /// Names of all indexed fields.
    [[nodiscard]] std::set<std::string> indexed_fields() const;

    // ---- segment level predicates (the reference implementation caches these on FieldInfos) ----

    /// `true` when at least one field stores term frequencies.
    [[nodiscard]] bool has_freqs() const;
    /// `true` when at least one field stores positions (the reference implementation's `has_prox`).
    [[nodiscard]] bool has_positions() const;
    /// `true` when at least one field stores payloads.
    [[nodiscard]] bool has_payloads() const;
    /// `true` when at least one field stores offsets.
    [[nodiscard]] bool has_offsets() const;
    /// `true` when at least one field stores norms.
    [[nodiscard]] bool has_norms() const;
    /// `true` when at least one field stores term vectors.
    [[nodiscard]] bool has_term_vectors() const;
    /// `true` when at least one field indexes postings (the reference implementation's `has_postings`).
    [[nodiscard]] bool has_postings() const;
    /// `true` when at least one field has doc values.
    [[nodiscard]] bool has_doc_values() const;
    /// `true` when at least one field has point values.
    [[nodiscard]] bool has_point_values() const;

private:
    FieldInfos() = default;

    std::vector<FieldInfo> fields_;
    /// Set by [`with_filter`]; when present it replaces `fields_` as the
    /// iteration view.
    std::optional<std::vector<FieldInfo>> visible_;
};

/// `.fnm` encoder/decoder.
///
/// Port of `Lucene94FieldInfosFormat`
/// (`codecs/lucene94/lucene94_field_infos_format.rs`).  The file is
/// `<segment>[_<suffix>].fnm`:
///
/// ```text
/// header      // CodecUtil::write_index_header("Lucene94FieldInfos", 2, id, suffix)
/// vint        // field count
/// per field, in number order:
///   string    name
///   vint      field number
///   byte      bitset: 1 storeTermVector, 2 omitNorms, 4 storePayloads,
///             8 softDeletes, 16 parent
///   byte      IndexOptions
///   byte      DocValuesType              // on-disk numbering, see below
///   byte      DocValuesSkipIndexType
///   long      doc values generation      // big endian, see below
///   map       attributes
///   vint      pointDimensionCount
///   vint      pointIndexDimensionCount   // only when the count is non-zero
///   vint      pointNumBytes              // only when the count is non-zero
///   vint      vectorDimension
///   byte      VectorEncoding
///   byte      VectorSimilarityFunction
/// footer
/// ```
///
/// `DocValuesType` is numbered on disk as `None=0, Numeric=1, Binary=2,
/// Sorted=3, SortedSet=4, SortedNumeric=5`, which is **not** this port's enum
/// order (`SortedNumeric` and `SortedSet` are swapped), so the two are mapped
/// explicitly.
///
/// **Deviation:** `docValuesGen` is written big endian here, while the reference implementation writes
/// it with `DataOutput::write_long`, which is little endian in both projects.
/// This port keeps every codec metadata payload big endian so a `.fnm`/`.si` pair
/// matches what Java Lucene writes; the data formats (BKD, postings) still follow
/// the port's little endian `DataOutput`, exactly like the reference implementation.
export struct Lucene94FieldInfosFormat {
    static constexpr std::string_view CODEC_NAME = "Lucene94FieldInfos";
    /// Only the current format version is written and read; an older header is
    /// rejected as `index_format_too_old` instead of being decoded.
    static constexpr int32_t VERSION_CURRENT = 2;
    /// Extension of the field infos file.
    static constexpr std::string_view FNM_EXTENSION = "fnm";

    /// File name for `segment` and `suffix`.
    [[nodiscard]] static std::string file_name(std::string_view segment, std::string_view suffix = "");

    [[nodiscard]] static VoidResult write(Directory &dir, std::string_view segment, std::string_view suffix,
                                          std::span<const uint8_t> id, const FieldInfos &infos,
                                          const IOContext &context);

    [[nodiscard]] static Result<FieldInfos> read(const Directory &dir, std::string_view segment,
                                                 std::string_view suffix, std::span<const uint8_t> id,
                                                 const IOContext &context);

    /// Writes the payload (everything between the header and the footer).
    [[nodiscard]] static VoidResult write_field_infos(DataOutput &out, const FieldInfos &infos);

    /// Reads the payload produced by [`write_field_infos`].
    [[nodiscard]] static Result<FieldInfos> parse_field_infos(DataInput &input);
};

} // namespace lucene
