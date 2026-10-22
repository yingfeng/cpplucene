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

export module lucene_core:block_tree_intersect_terms_enum;

import :types;
import std;

import :block_term_state;
import :block_tree_output_accumulator;
import :block_tree_segment_terms_enum;
import :block_tree_terms_constants;
import :bytes_ref;
import :bytes_ref_builder;
import :compiled_automaton;
import :compression_algorithm;
import :data_input;
import :error;
import :fst;
import :impacts_enum;
import :index_input;
import :postings_enum;
import :terms;
import :terms_enum;
import :transition;

namespace lucene {

/// Declared before the frame, which calls back into it; the definition follows
/// the frame, which it holds by value.
export class IntersectTermsEnum;

/// One block of the block tree, as seen by [`IntersectTermsEnum`].
///
/// Port of `IntersectTermsEnumFrame`
/// (`core/codecs/lucene90/block_tree/intersect_terms_enum_frame.rs`).
///
/// **Deviation:** like [`SegmentTermsEnumFrame`], the block loading and decoding
/// steps are statics taking the owning enum, because a
/// [`ByteArrayDataInput`] in this port borrows its bytes and so the frame has to
/// own them and re-seat the readers after every growth.
export class IntersectTermsEnumFrame {
public:
    IntersectTermsEnumFrame() = default;

    /// **Deviation:** the reference implementation's `new` returns `Result<Self>`; a C++ constructor
    /// cannot, so this is a factory.
    [[nodiscard]] static Result<IntersectTermsEnumFrame> create(size_t ord, FieldReader &fr);

    /// Re-points `frame.transition` at the transitions of `state`; with no
    /// transitions, `min` and `max` are set to `-1` so that the "label < min"
    /// and "label > max" checks fire immediately.
    static VoidResult set_state(AutomatonEnum &automaton, IntersectTermsEnumFrame &frame, int32_t state);

    static VoidResult load_next_floor_block(size_t frame_idx, IntersectTermsEnum &ite);
    static VoidResult load_from_frame_index_data(const BytesRef &frame_index_data, size_t frame_idx,
                                                 IntersectTermsEnum &ite);
    static VoidResult load_from_output_accumulator(size_t frame_idx, IntersectTermsEnum &ite);

    /// Decodes the next entry; `true` when it is a sub-block.
    static Result<bool> next(size_t frame_idx, IntersectTermsEnum &ite);
    static VoidResult next_leaf(size_t frame_idx, IntersectTermsEnum &ite);
    static Result<bool> next_non_leaf(size_t frame_idx, IntersectTermsEnum &ite);

    /// Decodes term metadata lazily, up to the current entry.
    static VoidResult decode_meta_data(size_t frame_idx, IntersectTermsEnum &ite);

    [[nodiscard]] Result<int32_t> get_term_block_ord() const;

private:
    /// Loads the block at `frame.fp`; `block_code` is the FST output the block
    /// was reached through, when it came from the index.
    static VoidResult load(size_t frame_idx, std::optional<int64_t> block_code, IntersectTermsEnum &ite);
    void reseat_readers();

public:
    /// Stack ordinal.
    size_t ord{0};

    /// File pointer of this block.
    size_t fp{0};
    size_t fp_orig{0};
    size_t fp_end{0};
    /// File pointer of the last sub-block loaded from this frame.
    size_t last_sub_fp{0};

    /// Automaton state on entering this block.
    int32_t state{0};
    /// Automaton state just before the last label.
    int32_t last_state{0};
    int32_t meta_data_upto{0};

    std::vector<uint8_t> suffix_bytes{};
    std::vector<uint8_t> suffix_length_bytes{};
    std::vector<uint8_t> stats_bytes{};
    std::vector<uint8_t> metadata_bytes{};
    std::vector<uint8_t> floor_bytes{};

    ByteArrayDataInput suffixes_reader{};
    ByteArrayDataInput suffix_lengths_reader{};
    int32_t stats_singleton_run_length{0};
    ByteArrayDataInput stats_reader{};
    ByteArrayDataInput floor_data_reader{};
    ByteArrayDataInput bytes_reader{};

    /// Length of the prefix shared by all terms in this block.
    size_t prefix{0};
    /// Number of entries (terms and sub-blocks) in this block.
    int32_t ent_count{0};
    /// Which entry will be read next.
    int32_t next_ent{0};
    /// `true` when this block is not a floor block, or is the last sub-block of
    /// a floor block.
    bool is_last_in_floor{false};
    /// `true` when all entries are terms.
    bool is_leaf_block{false};

    int32_t num_follow_floor_blocks{0};
    int32_t next_floor_label{0};

    /// The current automaton transition of this frame.
    Transition transition{};
    int32_t transition_index{0};
    int32_t transition_count{0};

    /// Index into `ite.arcs()` of the arc that led to this block.
    size_t arc{0};

    TermStateEnum term_state{IntBlockTermState{}};

    /// Number of outputs pushed by this frame, dropped when it is popped.
    size_t output_num{0};
    size_t start_byte_pos{0};
    /// Length of this entry's suffix.
    size_t suffix{0};
};

/// A [`TermsEnum`] that enumerates the terms of a block-tree field accepted by a
/// compiled automaton, without using the terms index on `next()`.
///
/// On initialization it loads the root block and scans forward to the start
/// term; each `next()` then scans forward until it finds a term matching the
/// current automaton transition.
///
/// Port of `IntersectTermsEnum`
/// (`core/codecs/lucene90/block_tree/intersect_terms_enum.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * as for [`SegmentTermsEnum`], the `FieldReader` is owned through a
///    `std::unique_ptr` and the FST exposes its own bytes reader, so the reference implementation's
///    `fst_reader` field is dropped;
///  * the term is kept in a [`BytesRefBuilder`] so that its length can be set
///    explicitly, which is what the reference implementation's `BytesRef` fields do.
export class IntersectTermsEnum final : public TermsEnum {
public:
    /// **Deviation:** the reference implementation's `new` returns `Result<Self>`; a C++ constructor
    /// cannot, so this is a factory.
    [[nodiscard]] static Result<std::unique_ptr<IntersectTermsEnum>> create(std::unique_ptr<FieldReader> fr,
                                                                           AutomatonEnum automaton,
                                                                           std::optional<BytesRef> common_suffix,
                                                                           const BytesRef *start_term);

    // ---- BytesRefIterator ----

    [[nodiscard]] Result<const BytesRef *> next() override;

    // ---- TermsEnum ----

    [[nodiscard]] Result<AttributeSource *> attributes() override;
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override;
    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> term) override;
    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> term) override;
    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) override;
    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> term) override;
    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t ord) override;
    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> term,
                                                   const TermStateEnum &state) override;
    [[nodiscard]] Result<const BytesRef *> term() const override;
    [[nodiscard]] Result<int64_t> ord() const override;
    [[nodiscard]] Result<int32_t> doc_freq() override;
    [[nodiscard]] Result<int64_t> total_term_freq() override;
    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> reuse,
                                                                           int32_t flags) override;
    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t flags) override;
    [[nodiscard]] Result<TermStateEnum> term_state() override;

    // ---- accessors used by the frames ----

    [[nodiscard]] std::vector<IntersectTermsEnumFrame> &stack() noexcept { return stack_; }
    [[nodiscard]] const std::vector<IntersectTermsEnumFrame> &stack() const noexcept { return stack_; }
    [[nodiscard]] std::vector<Arc> &arcs() noexcept { return arcs_; }
    [[nodiscard]] AutomatonEnum &automaton() noexcept { return automaton_; }
    [[nodiscard]] OutputAccumulator &output_accumulator() noexcept { return output_accumulator_; }
    [[nodiscard]] FieldReader &fr() noexcept { return *fr_; }
    [[nodiscard]] const FieldReader &fr() const noexcept { return *fr_; }
    [[nodiscard]] std::unique_ptr<IndexInput> &input() noexcept { return input_; }
    [[nodiscard]] BytesRefBuilder &term_builder() noexcept { return term_; }
    [[nodiscard]] size_t &current_frame() noexcept { return current_frame_; }
    [[nodiscard]] size_t &current_transition() noexcept { return current_transition_; }

private:
    friend class IntersectTermsEnumFrame;

    IntersectTermsEnum(std::unique_ptr<FieldReader> fr, AutomatonEnum automaton, std::optional<BytesRef> common_suffix);

    /// Lazily grows the frame stack so that `ord` exists.
    [[nodiscard]] VoidResult get_frame(size_t ord);
    /// Lazily grows the arc array so that `ord` exists; returns `ord`.
    [[nodiscard]] Result<size_t> get_arc(size_t ord);

    /// Pushes a frame for the sub-block the current frame points at.
    [[nodiscard]] Result<size_t> push_frame(int32_t state);

    /// The automaton state after consuming the current entry's suffix.
    [[nodiscard]] Result<int32_t> get_state();

    /// Scans forward from the root block until the term `target` is passed.
    [[nodiscard]] VoidResult seek_to_start_term(const BytesRef &target);

    /// Pops the finished frames (advancing floor blocks) and steps to the next
    /// entry; fails with `no_more_terms` at the end of the enumeration.
    [[nodiscard]] Result<bool> pop_push_next();

    /// [`next`], with the end of the enumeration reported as an error.
    [[nodiscard]] Result<const BytesRef *> next_();

    /// Copies the current entry's suffix into the term buffer.
    [[nodiscard]] VoidResult copy_term();

    std::unique_ptr<FieldReader> fr_;
    /// Cloned `.tim` input.
    std::unique_ptr<IndexInput> input_{};
    std::vector<IntersectTermsEnumFrame> stack_{};
    std::vector<Arc> arcs_{};
    AutomatonEnum automaton_;
    std::optional<BytesRef> common_suffix_{};
    size_t current_frame_{0};
    size_t current_transition_{0};
    BytesRefBuilder term_{};
    /// `FieldReader::root_code_`, kept for the first block load.
    OutputAccumulator output_accumulator_{};
    /// For the debug check that the start term is really skipped.
    std::optional<BytesRef> saved_start_term_{};
};

} // namespace lucene
