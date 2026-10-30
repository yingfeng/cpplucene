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

export module lucene_core:fst;

import :fst_output;
import :types;
import :error;

import std;

import :data_input;
import :data_output;
import :fst_bytes_reader;
import :fst_outputs;

namespace lucene {

// ---- arc flag bits ----

/// The arc's target is a final node.
export inline constexpr uint8_t BIT_FINAL_ARC = 1u << 0;
/// The arc is the last one leaving its node.
export inline constexpr uint8_t BIT_LAST_ARC = 1u << 1;
/// The target node is stored right after this arc.
export inline constexpr uint8_t BIT_TARGET_NEXT = 1u << 2;
/// The node has no outgoing arcs.
export inline constexpr uint8_t BIT_STOP_NODE = 1u << 3;
/// The arc carries an output.
export inline constexpr uint8_t BIT_ARC_HAS_OUTPUT = 1u << 4;
/// The arc carries the target's final output.
export inline constexpr uint8_t BIT_ARC_HAS_FINAL_OUTPUT = 1u << 5;

/// Node header value: fixed-length (sparse) arcs for binary search.
export inline constexpr uint8_t ARCS_FOR_BINARY_SEARCH = BIT_ARC_HAS_FINAL_OUTPUT;
/// Node header value: fixed-length dense arcs with a presence bit table.
export inline constexpr uint8_t ARCS_FOR_DIRECT_ADDRESSING = 1u << 6;
/// Node header value: continuous arcs, addressed by `label - firstLabel`.
export inline constexpr uint8_t ARCS_FOR_CONTINUOUS = ARCS_FOR_DIRECT_ADDRESSING + ARCS_FOR_BINARY_SEARCH;

/// Codec name of an FST file.
export inline constexpr std::string_view FILE_FORMAT_NAME = "FST";
/// First supported version (Lucene 7.0).
export inline constexpr int32_t VERSION_START = 6;
/// Version that switched to little endian arc data.
export inline constexpr int32_t VERSION_LITTLE_ENDIAN = 8;
/// Version that introduced continuous arcs.
export inline constexpr int32_t VERSION_CONTINUOUS_ARCS = 9;
/// Current format version.
export inline constexpr int32_t VERSION_CURRENT = VERSION_CONTINUOUS_ARCS;
/// The version Lucene 9.0 wrote.
export inline constexpr int32_t VERSION_90 = VERSION_LITTLE_ENDIAN;

/// A final virtual node with no arcs (never serialized).
export inline constexpr int64_t FINAL_END_NODE = -1;
/// A non-final virtual node with no arcs (never serialized).
export inline constexpr int64_t NON_FINAL_END_NODE = 0;
/// Label of the arc that marks a node as final.
export inline constexpr int32_t END_LABEL = -1;
/// Default for `maxBlockBits` when reading an on-heap FST.
export inline constexpr int32_t DEFAULT_MAX_BLOCK_BITS = 30;

/// `true` when all bits of `bit` are set in `flags`.
export inline constexpr bool has_flag(uint8_t flags, uint8_t bit) noexcept {
    return (flags & bit) != 0;
}



/// Which label widths an FST uses: 1, 2 or 4 bytes per label.
export enum class InputType : uint8_t {
    kByte1 = 0,
    kByte2 = 1,
    kByte4 = 2,
};

/// Reads one BYTE1/BYTE2/BYTE4 label.
///
/// BYTE2 fields written before [`VERSION_LITTLE_ENDIAN`] are big endian.
export [[nodiscard]] Result<int32_t> read_label(DataInput &input, InputType input_type, int32_t version);

/// Writes one BYTE1/BYTE2/BYTE4 label; the inverse of [`read_label`].
export [[nodiscard]] VoidResult write_label(DataOutput &out, InputType input_type, int32_t label);

/// A traversal slot describing one arc of one node.
///
/// Port of `Arc<T>` (`util/fst_impl/fst.rs`).  The `output` and
/// `next_final_output` values are type erased like the rest of the FST API.
export class Arc {
public:
    Arc() = default;

    /// An arc whose output slots are pre-filled with this algebra's NO_OUTPUT.
    [[nodiscard]] static Arc with_outputs(FstOutput output, FstOutput next_final_output);

    [[nodiscard]] bool is_last() const noexcept { return has_flag(flags_, BIT_LAST_ARC); }
    [[nodiscard]] bool is_final() const noexcept { return has_flag(flags_, BIT_FINAL_ARC); }
    [[nodiscard]] bool has_output() const noexcept { return has_flag(flags_, BIT_ARC_HAS_OUTPUT); }
    [[nodiscard]] bool has_final_output() const noexcept { return has_flag(flags_, BIT_ARC_HAS_FINAL_OUTPUT); }
    [[nodiscard]] bool is_stop_node() const noexcept { return has_flag(flags_, BIT_STOP_NODE); }
    [[nodiscard]] bool target_is_next() const noexcept { return has_flag(flags_, BIT_TARGET_NEXT); }

    [[nodiscard]] int32_t label() const noexcept { return label_; }
    [[nodiscard]] int64_t target() const noexcept { return target_; }
    [[nodiscard]] uint8_t flags() const noexcept { return flags_; }
    [[nodiscard]] int64_t next_arc() const noexcept { return next_arc_; }
    [[nodiscard]] int32_t arc_idx() const noexcept { return arc_idx_; }
    [[nodiscard]] uint8_t node_flags() const noexcept { return node_flags_; }
    [[nodiscard]] int64_t pos_arcs_start() const noexcept { return pos_arcs_start_; }
    [[nodiscard]] int32_t bytes_per_arc() const noexcept { return bytes_per_arc_; }
    [[nodiscard]] int32_t num_arcs() const noexcept { return num_arcs_; }
    [[nodiscard]] int32_t first_label() const noexcept { return first_label_; }
    [[nodiscard]] int64_t bit_table_start() const noexcept { return bit_table_start_; }
    [[nodiscard]] int32_t presence_index() const noexcept { return presence_index_; }

    [[nodiscard]] const FstOutput &output() const noexcept { return output_; }
    [[nodiscard]] const FstOutput &next_final_output() const noexcept { return next_final_output_; }
    /// Mutable access, needed by `FstOutputs::add_assign` which consumes the
    /// arc's output as a scratch slot.
    [[nodiscard]] FstOutput &output_mut() noexcept { return output_; }
    [[nodiscard]] FstOutput &next_final_output_mut() noexcept { return next_final_output_; }

    void set_label(int32_t label) noexcept { label_ = label; }
    void set_output(FstOutput output) { output_ = std::move(output); }
    void set_target(int64_t target) noexcept { target_ = target; }
    void set_flags(uint8_t flags) noexcept { flags_ = flags; }
    void set_next_final_output(FstOutput output) { next_final_output_ = std::move(output); }
    void set_next_arc(int64_t next_arc) noexcept { next_arc_ = next_arc; }
    void set_node_flags(uint8_t node_flags) noexcept { node_flags_ = node_flags; }
    void set_pos_arcs_start(int64_t pos) noexcept { pos_arcs_start_ = pos; }
    void set_bytes_per_arc(int32_t bytes_per_arc) noexcept { bytes_per_arc_ = bytes_per_arc; }
    void set_arc_idx(int32_t arc_idx) noexcept { arc_idx_ = arc_idx; }
    void set_num_arcs(int32_t num_arcs) noexcept { num_arcs_ = num_arcs; }
    void set_first_label(int32_t first_label) noexcept { first_label_ = first_label; }
    void set_bit_table_start(int64_t start) noexcept { bit_table_start_ = start; }
    void set_presence_index(int32_t presence_index) noexcept { presence_index_ = presence_index; }

    /// Copies every field from `other`.
    void copy_from(const Arc &other);

    [[nodiscard]] std::string to_string() const;

private:
    int32_t label_{0};
    FstOutput output_{};
    int64_t target_{0};
    uint8_t flags_{0};
    FstOutput next_final_output_{};
    int64_t next_arc_{0};
    uint8_t node_flags_{0};
    int32_t bytes_per_arc_{0};
    int64_t pos_arcs_start_{0};
    int32_t arc_idx_{0};
    int32_t num_arcs_{0};
    int64_t bit_table_start_{0};
    int32_t first_label_{0};
    int32_t presence_index_{0};
};

/// The FST metadata written at the start of the metadata stream.
///
/// Port of `FSTMetadata` (`util/fst_impl/fst.rs`).
export struct FSTMetadata {
    InputType input_type{InputType::kByte1};
    /// The output algebra; not owned.
    const FstOutputs *outputs{nullptr};
    int32_t version{VERSION_CURRENT};
    /// If set, the FST accepts the empty string and produces this output.
    std::optional<FstOutput> empty_output{};
    int64_t start_node{0};
    int64_t num_bytes{0};

    /// Writes the codec header followed by the metadata.
    ///
    /// The empty output is serialized with [`FstOutputs::write_final_output`]
    /// and then **reversed**, because the FST reader can only read backwards.
    [[nodiscard]] VoidResult save(DataOutput &meta_out) const;

    /// Reads metadata written by [`save`].
    [[nodiscard]] static Result<FSTMetadata> read(DataInput &meta_in, const FstOutputs &outputs);
};

/// `true` when the arc points at a real node (`target > 0`).
export [[nodiscard]] inline bool target_has_arcs(const Arc &arc) noexcept {
    return arc.target() > 0;
}

/// Rewrites `arc` into the virtual END_LABEL arc of a final `follow` arc.
///
/// Returns `false` (and leaves `arc` untouched) when `follow` is not final.
export bool read_end_arc(const Arc &follow, Arc &arc);

/// An immutable FST over an already-loaded byte store.
///
/// Port of `FST<O, F>` (`util/fst_impl/fst.rs`), reduced to the parts that do
/// not depend on the generic store: the metadata plus one [`FstBytesReader`].
/// Arc decoding (`read_next_arc`, `find_target_arc`, ...) is added on top of
/// this class.
export class FST {
public:
    FST(FSTMetadata metadata, std::unique_ptr<FstBytesReader> reader);

    [[nodiscard]] const FSTMetadata &metadata() const noexcept { return metadata_; }
    [[nodiscard]] int64_t start_node() const noexcept { return metadata_.start_node; }
    [[nodiscard]] int64_t num_bytes() const noexcept { return metadata_.num_bytes; }
    [[nodiscard]] const FstOutputs &outputs() const { return *metadata_.outputs; }
    /// The reader this FST walks itself with.
    ///
    /// The overloads exist so that read-only helpers (`FstUtil`, `TopNSearcher`)
    /// can take a `const FST &` and still reposition the cursor.  An `FST` is
    /// therefore not safe to traverse concurrently; callers that need parallel
    /// lookups construct one `FST` per thread.
    [[nodiscard]] FstBytesReader &reader() noexcept { return *reader_; }
    [[nodiscard]] FstBytesReader &reader() const noexcept { return *reader_; }

    /// Labels use the width recorded in the metadata.
    [[nodiscard]] Result<int32_t> read_label(DataInput &input) const;

    /// Fills the virtual start arc, i.e. an empty incoming arc to the start
    /// node, reusing the caller's arc.
    void get_first_arc(Arc &arc) const;

    // ---- arc decoding ----

    /// Follows `follow` and reads the first arc of its target.
    [[nodiscard]] VoidResult read_first_target_arc(const Arc &follow, Arc &arc, FstBytesReader &reader) const;

    /// Follows `follow` and reads the last arc of its target.
    [[nodiscard]] VoidResult read_last_target_arc(const Arc &follow, Arc &arc, FstBytesReader &reader) const;

    /// Reads the first arc of the node at `node_address`.
    [[nodiscard]] VoidResult read_first_real_target_arc(int64_t node_address, Arc &arc, FstBytesReader &reader) const;

    /// Advances `arc` to the next arc of the same node; fails when `arc` is last.
    [[nodiscard]] VoidResult read_next_arc(Arc &arc, FstBytesReader &reader) const;

    /// Advances `arc` to the next arc; must not be called when `arc` is last.
    [[nodiscard]] VoidResult read_next_real_arc(Arc &arc, FstBytesReader &reader) const;

    /// Decodes the arc whose flags byte was already read into `arc.flags`.
    [[nodiscard]] VoidResult read_arc(Arc &arc, FstBytesReader &reader) const;

    [[nodiscard]] VoidResult read_arc_by_index(Arc &arc, FstBytesReader &reader, int32_t idx) const;
    [[nodiscard]] VoidResult read_arc_by_continuous(Arc &arc, FstBytesReader &reader, int32_t range_index) const;
    [[nodiscard]] VoidResult read_arc_by_direct_addressing(Arc &arc, FstBytesReader &reader,
                                                           int32_t range_index) const;
    [[nodiscard]] VoidResult read_arc_by_direct_addressing_with_presence_index(Arc &arc, FstBytesReader &reader,
                                                                                int32_t range_index,
                                                                                int32_t presence_index) const;
    [[nodiscard]] VoidResult read_last_arc_by_direct_addressing(Arc &arc, FstBytesReader &reader) const;
    [[nodiscard]] VoidResult read_last_arc_by_continuous(Arc &arc, FstBytesReader &reader) const;

    /// `true` when the target of `follow` uses fixed-length arcs.
    [[nodiscard]] Result<bool> is_expanded_target(const Arc &follow, FstBytesReader &reader) const;

    /// Finds the arc leaving `follow` whose label is `label_to_match`.
    ///
    /// `follow == nullptr` means "the node `arc` currently points at".  Returns
    /// `false` when the arc does not exist; `arc` then holds the position where
    /// a caller like `seekFloor` needs to resume.
    [[nodiscard]] Result<bool> find_target_arc(int32_t label_to_match, const Arc *follow, Arc &arc,
                                               FstBytesReader &reader) const;

    /// Skips a variable-length node up to and including its last arc.
    [[nodiscard]] VoidResult seek_to_next_node(FstBytesReader &reader) const;

private:
    /// Reads the node header (or the first arc's flags) at `node_address`.
    [[nodiscard]] VoidResult read_first_arc_info(int64_t node_address, Arc &arc, FstBytesReader &reader) const;
    /// Records where the presence bits of a direct-addressing node start.
    [[nodiscard]] VoidResult read_presence_bytes(Arc &arc, FstBytesReader &reader) const;
    /// Reads an arc's target that is stored as a vlong.
    [[nodiscard]] Result<int64_t> read_unpacked_node_target(FstBytesReader &reader) const;

    FSTMetadata metadata_;
    std::unique_ptr<FstBytesReader> reader_;
};

} // namespace lucene
