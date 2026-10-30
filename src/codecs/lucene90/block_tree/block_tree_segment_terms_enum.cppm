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

export module lucene_core:block_tree_segment_terms_enum;

import :types;
import std;

import :block_term_state;
import :compression_algorithm;
import :block_tree_output_accumulator;
import :bytes_ref;
import :bytes_ref_builder;
import :compiled_automaton;
import :data_input;
import :error;
import :field_infos;
import :fst;
import :index_input;
import :postings_enum;
import :impacts_enum;
import :postings_reader_base;
import :terms;
import :terms_enum;

namespace lucene {

/// What [`FieldReader`] and [`SegmentTermsEnum`] need from the owning
/// `TermsReader`.
///
/// **Deviation:** the reference implementation gives `FieldReader` an `Option<Arc<TermsReader>>` and
/// calls back into it (`parent.postings_reader`, `parent.terms_in`,
/// `parent.segment`, `parent.version`).  `TermsReader` owns the field readers,
/// so the two types are mutually recursive and cannot be split into two C++
/// module partitions; this interface is the narrow, acyclic end of that
/// relationship, implemented by `TermsReader` in
/// `:block_tree_terms_reader`.
export class BlockTreeTermsParent {
public:
    virtual ~BlockTreeTermsParent() = default;

    /// A clone of the `.tim` input, positioned anywhere.
    [[nodiscard]] virtual Result<std::unique_ptr<IndexInput>> clone_terms_input() const = 0;

    /// The postings reader that decodes per-term metadata.
    [[nodiscard]] virtual PostingsReaderBase &postings_reader() = 0;

    /// The segment name, used by the block statistics.
    [[nodiscard]] virtual const std::string &segment_name() const = 0;

    /// The block tree format version of the segment.
    [[nodiscard]] virtual int32_t version() const = 0;
};

/// The term dictionary of one field: the FST index plus its root block.
///
/// Port of `FieldReader` (`core/codecs/lucene90/block_tree/field_reader.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * `Arc<TermsReader>` becomes [`BlockTreeTermsParent`] (see above);
///  * `Arc<FieldInfo>` becomes a `std::shared_ptr<const FieldInfo>`;
///  * the reference implementation's `TryClone` (used to hand a copy to each `SegmentTermsEnum`)
///    becomes [`clone_for_enum`], which shares the FST instead of deep copying
///    it – the same thing `Arc::clone` did;
///  * the fields are public: the reference implementation declares them `pub(crate)` and the frame /
///    enum read them directly, which a C++ module partition cannot express.
export class FieldReader : public Terms {
public:
    FieldReader() = default;

    /// Reads the FST metadata and remembers what [`init_field_reader`] needs.
    ///
    /// **Deviation:** the reference implementation's constructor returns `Result<Self>`; a C++
    /// constructor cannot, so this is a factory.
    [[nodiscard]] static Result<std::unique_ptr<FieldReader>> create(
            std::shared_ptr<const FieldInfo> field_info, int64_t num_terms, BytesRef root_code,
            int64_t sum_total_term_freq, int64_t sum_doc_freq, int32_t doc_count, int64_t index_start_fp,
            DataInput &meta_in, BytesRef min_term, BytesRef max_term);

    /// Opens the FST index over `index_in` and computes the root block pointer.
    ///
    /// **Deviation:** the reference implementation keeps the FST off heap over the (memory mapped)
    /// `.tip` input; the [`FST`] of this port reads from an owned byte store, so
    /// the `num_bytes` bytes of the index are copied into memory once per field.
    [[nodiscard]] static VoidResult init_field_reader(IndexInput &index_in, FieldReader &reader);

    /// The copy handed to a [`SegmentTermsEnum`]; requires [`init_field_reader`].
    [[nodiscard]] Result<std::unique_ptr<FieldReader>> clone_for_enum() const;

    // ---- Terms ----

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override;

    /// Walks the blocks instead of filtering [`iterator`], which is what
    /// The reference implementation's `FieldReader::intersect` does through
    /// `IntersectTermsEnum`.  Defined in `:block_tree_intersect_terms_enum`,
    /// which imports this partition.
    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> intersect(const CompiledAutomaton &compiled,
                                                              const BytesRef *start_term) const override;

    [[nodiscard]] Result<int64_t> size() const override { return num_terms_; }
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override { return sum_total_term_freq_; }
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override { return sum_doc_freq_; }
    [[nodiscard]] Result<int32_t> doc_count() const override { return doc_count_; }
    [[nodiscard]] bool has_freqs() const override;
    [[nodiscard]] bool has_offsets() const override;
    [[nodiscard]] bool has_positions() const override;
    [[nodiscard]] bool has_payloads() const override;
    [[nodiscard]] Result<std::optional<BytesRef>> get_min() const override;
    [[nodiscard]] Result<std::optional<BytesRef>> get_max() const override;
    [[nodiscard]] Result<std::string> get_stats() const override;
    [[nodiscard]] std::string to_string() const;

    /// Reads a block tree output vlong; only the current (MSB) format exists.
    [[nodiscard]] Result<int64_t> read_vlong_output(DataInput &input) const;

    [[nodiscard]] const std::shared_ptr<const FieldInfo> &field_info() const noexcept { return field_info_; }
    [[nodiscard]] BlockTreeTermsParent *parent() noexcept { return parent_; }
    [[nodiscard]] const BlockTreeTermsParent *parent() const noexcept { return parent_; }
    void set_parent(BlockTreeTermsParent *parent) noexcept { parent_ = parent; }
    [[nodiscard]] FST *index() noexcept { return index_.get(); }
    [[nodiscard]] const FST *index() const noexcept { return index_.get(); }
    /// `metadatas` of the pending FST, kept until [`init_field_reader`] runs.
    [[nodiscard]] bool has_pending_index() const noexcept { return pending_index_start_fp_.has_value(); }

public:
    // The trailing underscores keep these clear of the `Terms` accessors of the
    // same name (`sum_total_term_freq()` & co).
    int64_t num_terms_{0};
    std::shared_ptr<const FieldInfo> field_info_{};
    int64_t sum_total_term_freq_{0};
    int64_t sum_doc_freq_{0};
    int32_t doc_count_{0};
    int64_t root_block_fp_{0};
    BytesRef root_code_{};
    BytesRef min_term_{};
    BytesRef max_term_{};
    BlockTreeTermsParent *parent_{nullptr};
    std::shared_ptr<FST> index_{};
    /// `TmpData`: the metadata read by [`create`], consumed by
    /// [`init_field_reader`].
    std::optional<FSTMetadata> pending_metadata_{};
    std::optional<int64_t> pending_index_start_fp_{};
    BytesRef pending_root_code_{};
};

/// The term enumerator that owns the frame stack.  Defined after the frame,
/// which it holds by value, and which in turn calls back into it.  The
/// declaration carries the `export` because a class may only be exported once.
export class SegmentTermsEnum;

/// One block of the block tree.
///
/// Port of `SegmentTermsEnumFrame`
/// (`core/codecs/lucene90/block_tree/segment_terms_enum_frame.rs`).
///
/// **Deviation:** the reference implementation's `ByteArrayDataInput<Vec<u8>>` owns the bytes it
/// reads; this port's [`ByteArrayDataInput`] only borrows, so the frame owns the
/// buffers and re-seats the readers after every growth ([`reseat_readers`]).
export class SegmentTermsEnumFrame {
public:
    SegmentTermsEnumFrame() = default;

    /// **Deviation:** the reference implementation's `SegmentTermsEnumFrame::new` returns
    /// `Result<Self>`; a C++ constructor cannot, so this is a factory.
    [[nodiscard]] static Result<SegmentTermsEnumFrame> create(int32_t ord, FieldReader &fr);

    [[nodiscard]] VoidResult set_floor_data(const OutputAccumulator &output_accumulator);
    [[nodiscard]] Result<int32_t> get_term_block_ord() const;

    static Result<bool> load_next_floor_block(size_t frame_idx, SegmentTermsEnum &ste);
    static VoidResult prefetch_block(size_t frame_idx, SegmentTermsEnum &ste);
    static Result<bool> load_block(size_t frame_idx, SegmentTermsEnum &ste);
    [[nodiscard]] VoidResult rewind();

    static Result<bool> next(size_t frame_idx, SegmentTermsEnum &ste);
    static VoidResult next_leaf(size_t frame_idx, SegmentTermsEnum &ste);
    static Result<bool> next_non_leaf(size_t frame_idx, SegmentTermsEnum &ste);

    static VoidResult scan_to_floor_frame(size_t frame_idx, SegmentTermsEnum &ste);
    static VoidResult scan_to_floor_frame_with_target(size_t frame_idx, std::span<const uint8_t> target,
                                                     SegmentTermsEnum &ste, bool use_target);
    static VoidResult decode_meta_data(size_t frame_idx, SegmentTermsEnum &ste);
    static VoidResult scan_to_sub_block(size_t frame_idx, size_t sub_fp, SegmentTermsEnum &ste);

    static Result<SeekStatus> scan_to_term(size_t frame_idx, std::span<const uint8_t> target, bool exact_only,
                                          SegmentTermsEnum &ste);
    static Result<SeekStatus> scan_to_term_leaf(size_t frame_idx, std::span<const uint8_t> target, bool exact_only,
                                               SegmentTermsEnum &ste);
    static Result<SeekStatus> binary_search_term_leaf(size_t frame_idx, std::span<const uint8_t> target,
                                                      bool exact_only, SegmentTermsEnum &ste);
    static Result<SeekStatus> scan_to_term_non_leaf(size_t frame_idx, std::span<const uint8_t> target,
                                                    bool exact_only, SegmentTermsEnum &ste);
    static VoidResult fill_term(size_t frame_idx, SegmentTermsEnum &ste);

private:
    static bool prefix_matches(size_t frame_idx, std::span<const uint8_t> target, const SegmentTermsEnum &ste);
    void reseat_readers();

public:
    /// Stack ordinal; -1 for the static frame.
    int32_t ord{0};

    bool has_terms{false};
    bool has_terms_orig{false};
    bool is_floor{false};

    std::optional<size_t> arc{};

    /// File pointer this block was loaded from.
    size_t fp{0};
    size_t fp_orig{0};
    size_t fp_end{0};
    /// Suffix bytes of the block, for the statistics.
    int64_t total_suffix_bytes{0};

    std::vector<uint8_t> suffix_bytes{};
    std::vector<uint8_t> suffix_length_bytes{};
    std::vector<uint8_t> stats_bytes{};
    std::vector<uint8_t> metadata_bytes{};
    std::vector<uint8_t> floor_bytes{};

    ByteArrayDataInput suffixes_reader{};
    ByteArrayDataInput suffix_lengths_reader{};
    ByteArrayDataInput stats_reader{};
    ByteArrayDataInput bytes_reader{};
    ByteArrayDataInput floor_data_reader{};

    int32_t stats_singleton_run_length{0};
    size_t rewind_pos{0};

    /// Length of the prefix shared by all terms in this block.
    size_t prefix_length{0};
    /// Number of entries (term or sub-block) in this block.
    int32_t ent_count{0};
    /// Which term to read next, or -1 when the block is not loaded yet.
    int32_t next_ent{0};

    bool is_last_in_floor{false};
    /// Whether all entries are terms.
    bool is_leaf_block{false};
    /// Whether all entries have the same length.
    bool all_equal{false};

    std::optional<size_t> last_sub_fp{0};

    int32_t next_floor_label{0};
    int32_t num_follow_floor_blocks{0};

    /// Next metadata entry to decode; metadata is decoded lazily.
    int32_t meta_data_upto{0};

    /// `TermStateEnum` has no default constructor, so the frame starts from an
    /// empty `IntBlockTermState` and the constructor replaces it with the one
    /// the postings reader hands out.  The reference implementation does the same through
    /// `FieldReader::new_term_state()`.
    TermStateEnum state{IntBlockTermState{}};
    CompressionAlgorithm compression_alg{CompressionAlgorithm::NoCompression};

    size_t start_byte_pos{0};
    size_t suffix_length{0};
    int64_t sub_code{0};
};

/// Iterates the terms of one field by walking the FST index down to the block
/// that holds the term, then scanning the block.
///
/// Port of `SegmentTermsEnum`
/// (`core/codecs/lucene90/block_tree/segment_terms_enum.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the `FieldReader` is owned through a `std::unique_ptr` (the reference implementation stores it
///    by value and clones it in `FieldReader::iterator`), so that
///    [`FieldReader::clone_for_enum`] can hand one over without a deep copy of
///    the FST;
///  * `fst_reader` is dropped: this port's [`FST`] owns its bytes reader and
///    exposes it through `FST::reader()`, which is what the arc walkers take;
///  * actions that cannot be expressed as `fn(&mut self) -> Result<_>` chains
///    (the recursive sub-frame descent) use the same frame index arguments as
///    The reference implementation's `SegmentTermsEnumFrame::*` statics.
class SegmentTermsEnum : public TermsEnum {
public:
    /// Static frame index: `stack[0]` holds the seek-by-term-state frame.
    static constexpr size_t STATIC_FRAME_IDX = 0;

    /// **Deviation:** the reference implementation's `SegmentTermsEnum::new` returns
    /// `Result<Self>`; a C++ constructor cannot fail, so this is a factory that
    /// also builds the static frame.
    [[nodiscard]] static Result<std::unique_ptr<SegmentTermsEnum>> create(std::unique_ptr<FieldReader> fr);

    [[nodiscard]] VoidResult init_index_input();

    /// Pushes a frame for the sub-block at `target_fp`, reusing the frame above
    /// the current one when possible; `arc` is the FST arc leading to the
    /// block.  Returns the stack index of the (re)used frame.
    [[nodiscard]] Result<size_t> push_frame(std::optional<size_t> arc, size_t target_fp, size_t prefix_len);
    [[nodiscard]] Result<size_t> push_frame_with_data(std::optional<size_t> arc, const BytesRef &term_bytes,
                                                      size_t prefix_len);
    /// Pushes the frame for the block the accumulated output points at.
    [[nodiscard]] Result<size_t> push_frame_with_length(std::optional<size_t> arc, size_t length);
    /// Lazily grows the frame stack; `ord` is the frame ordinal (`0` is the
    /// first sub-frame, the static frame is `-1`).
    [[nodiscard]] Result<size_t> get_frame(size_t ord);
    /// Lazily grows the arc array.
    [[nodiscard]] Result<size_t> get_arc(size_t ord);

    // ---- TermsEnum ----

    [[nodiscard]] Result<const BytesRef *> next() override;
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

    [[nodiscard]] std::string to_string() const;

    // ---- accessors used by the frames ----

    [[nodiscard]] FieldReader &fr() noexcept { return *fr_; }
    [[nodiscard]] const FieldReader &fr() const noexcept { return *fr_; }
    [[nodiscard]] BytesRefBuilder &term_builder() noexcept { return term_; }
    [[nodiscard]] const BytesRefBuilder &term_builder() const noexcept { return term_; }
    [[nodiscard]] std::span<const uint8_t> term_bytes() const noexcept { return term_.get_bytes_ref().as_slice(); }
    [[nodiscard]] std::deque<SegmentTermsEnumFrame> &stack() noexcept { return stack_; }
    [[nodiscard]] const std::deque<SegmentTermsEnumFrame> &stack() const noexcept { return stack_; }
    [[nodiscard]] std::unique_ptr<IndexInput> &input() noexcept { return input_; }
    [[nodiscard]] OutputAccumulator &output_accumulator() noexcept { return output_accumulator_; }
    [[nodiscard]] std::vector<Arc> &arcs() noexcept { return arcs_; }
    [[nodiscard]] bool &term_exists() noexcept { return term_exists_; }
    [[nodiscard]] size_t &current_frame_idx() noexcept { return current_frame_idx_; }
    [[nodiscard]] bool &eof() noexcept { return eof_; }
    [[nodiscard]] int32_t &target_before_current_length() noexcept { return target_before_current_length_; }
    [[nodiscard]] size_t &valid_index_prefix() noexcept { return valid_index_prefix_; }

public:
    /// Outcome of the last [`prepare_seek_exact`]; the reference implementation's
    /// `PrepareSeekStatus`.
    enum class PrepareStatus : uint8_t {
        /// The target is definitely absent.
        NotFound,
        /// The target was found without touching the block.
        Found,
        /// The target may be in the block the walk ended on.
        Pending,
    };

private:
    friend class SegmentTermsEnumFrame;

    /// The reference implementation's `prepare_seek_exact(target, prefetch)`; the trait method always
    /// asks for a prefetch.
    [[nodiscard]] Result<bool> prepare_seek_exact_with_prefetch(std::span<const uint8_t> target, bool prefetch);

    std::unique_ptr<FieldReader> fr_;
    /// Lazily cloned `.tim` input (the reference implementation: `input: Option<I>`).
    std::unique_ptr<IndexInput> input_{};
    /// `stack[0]` is the static frame used for seek by term state.
    /// A `deque`, not a `vector`: a seek pushes one frame per byte of the term and
    /// a frame owns five buffers, so growing a vector reallocated and moved all of
    /// them (and that move was one of the hottest allocator callers of a lookup).
    std::deque<SegmentTermsEnumFrame> stack_{};
    size_t current_frame_idx_{STATIC_FRAME_IDX};
    bool term_exists_{false};
    /// Length of the prefix the FST walk already matched; `-1` means "equal to
    /// the static frame", like the reference implementation's `i32` sentinel.
    int32_t target_before_current_length_{-1};
    OutputAccumulator output_accumulator_{};
    size_t valid_index_prefix_{0};
    bool eof_{false};
    BytesRefBuilder term_{};
    std::vector<Arc> arcs_{};
    /// `PrepareSeekStatus` of the last [`prepare_seek_exact`].
    PrepareStatus prepare_status_{PrepareStatus::NotFound};
};

} // namespace lucene
