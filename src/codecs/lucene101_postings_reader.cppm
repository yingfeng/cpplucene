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

export module lucene_core:lucene101_postings_reader;

import :types;
import std;

import :bit_util;
import :block_term_state;
import :codec_util;
import :data_input;
import :error;
import :field_infos;
import :for_delta_util;
import :for_util;
import :impact;
import :impacts_enum;
import :impacts_source;
import :index_input;
import :index_types;
import :lucene101_postings_format;
import :pfor_util;
import :posting_decoding_util;
import :postings_enum;
import :postings_reader_base;
import :postings_util;
import :segment_info;
import :segment_read_state;

namespace lucene {

/// A growable impacts list that remembers how many entries are valid, so
/// decoding can reuse the segment-wide maximum capacity across calls.
///
/// Port of `MutableImpactList` (`lucene101_postings_reader.rs`).
export struct MutableImpactList {
    /// Number of valid entries in [`impacts`].
    size_t length{0};
    /// Decoded impacts; entries past [`length`] are stale.
    std::vector<Impact> impacts;
};

/// Decodes impacts previously written by `write_impacts`.
///
/// `in` is consumed until its end; `reuse` must be empty or hold at least the
/// number of impacts that `in` encodes.
[[nodiscard]] VoidResult read_impacts(ByteArrayDataInput &in, MutableImpactList &reuse);

/// Reads a vint that was written by `write_vint15`.
///
/// See also `write_vint15`.
[[nodiscard]] Result<int32_t> read_vint15(DataInput &input);

/// Reads a vlong that was written by `write_vlong15`.
///
/// See also `write_vlong15`.
[[nodiscard]] Result<int64_t> read_vlong15(DataInput &input);

/// Turns `buffer[0 .. count)` into a prefix sum, starting from `base`.
export void prefix_sum(std::span<int32_t> buffer, size_t count, int32_t base);

/// `sum(arr[start .. end])`, as the reference implementation computes it for skip-data validation.
[[nodiscard]] int32_t sum_over_range(std::span<const int32_t> arr, size_t start, size_t end);

/// Prefetches the postings of `state` unless `doc_in` is already positioned
/// there, which suggests the caller is streaming the whole inverted index (for
/// example while merging) and should let the read-ahead logic do its work.
[[nodiscard]] VoidResult prefetch_postings(IndexInput &doc_in, const IntBlockTermState &state);

/// The lazy, per-level impacts of a postings enum.
///
/// Port of `ImpactsImpl` (`lucene101_postings_reader.rs`).
export class ImpactsImpl : public Impacts {
public:
    ImpactsImpl(bool index_has_freq, int32_t level0_last_doc_id, int32_t level1_last_doc_id,
                std::optional<std::span<const uint8_t>> level0_serialized_impacts,
                std::optional<std::span<const uint8_t>> level1_serialized_impacts,
                size_t max_num_impacts_at_level0, size_t max_num_impacts_at_level1);

    /// Decodes the impacts of one level out of its serialized form.
    [[nodiscard]] static Result<MutableImpactList> decode(std::span<const uint8_t> serialized,
                                                         size_t level_impacts_len);

    [[nodiscard]] size_t num_levels() const override;
    [[nodiscard]] int32_t get_doc_id_upto(size_t level) const override;
    [[nodiscard]] Result<std::vector<Impact>> get_impacts(size_t level) const override;

    /// The fallback impact the reference implementation returns when the field has no frequencies.
    [[nodiscard]] static Impact default_impact() noexcept { return Impact{std::numeric_limits<int32_t>::max(), 1}; }

private:
    bool index_has_freq_;
    int32_t level0_last_doc_id_;
    int32_t level1_last_doc_id_;
    std::optional<std::span<const uint8_t>> level0_serialized_impacts_;
    std::optional<std::span<const uint8_t>> level1_serialized_impacts_;
    size_t max_num_impacts_at_level0_;
    size_t max_num_impacts_at_level1_;
};

/// Reader for [`Lucene101PostingsFormat`].
///
/// Port of `Lucene101PostingsReader`
/// (`core/codecs/lucene101/lucene101_postings_reader.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the `I: IndexInput` type parameter becomes `std::unique_ptr<IndexInput>`;
///  * the reference implementation's `panic::catch_unwind` + `IOUtils::close_while_handling_exception`
///    error handling becomes "close what was opened and return the first
///    error", because neither mechanism exists in this port yet;
///  * the `ReadAdvice::Normal` IO context of the docs file is not distinguished.
///
/// **Not ported yet:** `postings` / `impacts` need `BlockPostingsEnum`, so they
/// currently report `kNotImplemented`.
export class Lucene101PostingsReader : public PostingsReaderBase {
public:
    /// Opens and validates `.psm`, `.doc` (and `.pos` / `.pay` when the segment
    /// has them).
    [[nodiscard]] static Result<std::unique_ptr<Lucene101PostingsReader>> create(const SegmentReadState &state,
                                                                                const SegmentInfo &segment_info);

    [[nodiscard]] VoidResult init(IndexInput &terms_in, const SegmentReadState &state,
                                  const SegmentInfo &segment_info) override;

    [[nodiscard]] Result<TermStateEnum> new_term_state() const override;

    [[nodiscard]] VoidResult decode_term(DataInput &input, const FieldInfo &field_info, TermStateEnum &state,
                                         bool absolute) const override;

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings(const FieldInfo &field_info,
                                                                 const TermStateEnum &state,
                                                                 std::unique_ptr<PostingsEnum> reuse,
                                                                 int32_t flags) const override;

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(const FieldInfo &field_info,
                                                               const TermStateEnum &state,
                                                               int32_t flags) const override;

    /// Verifies the CRC32 of every postings file; costly, so only for checks.
    [[nodiscard]] VoidResult check_integrity() const override;

    [[nodiscard]] VoidResult close() override;

    [[nodiscard]] std::string to_string() const override;

    // ---- test / diagnostics accessors ----

    [[nodiscard]] int32_t max_num_impacts_at_level0() const noexcept { return max_num_impacts_at_level0_; }
    [[nodiscard]] int32_t max_impact_num_bytes_at_level0() const noexcept { return max_impact_num_bytes_at_level0_; }
    [[nodiscard]] int32_t max_num_impacts_at_level1() const noexcept { return max_num_impacts_at_level1_; }
    [[nodiscard]] int32_t max_impact_num_bytes_at_level1() const noexcept { return max_impact_num_bytes_at_level1_; }
    [[nodiscard]] bool has_positions() const noexcept { return pos_in_ != nullptr; }
    [[nodiscard]] bool has_payloads() const noexcept { return pay_in_ != nullptr; }
    [[nodiscard]] IndexInput *doc_input() noexcept { return doc_in_.get(); }
    [[nodiscard]] IndexInput *pos_input() noexcept { return pos_in_.get(); }
    [[nodiscard]] IndexInput *pay_input() noexcept { return pay_in_.get(); }

private:
    Lucene101PostingsReader() = default;

    [[nodiscard]] VoidResult close_quietly() noexcept;

    std::unique_ptr<IndexInput> doc_in_;
    std::unique_ptr<IndexInput> pos_in_;
    std::unique_ptr<IndexInput> pay_in_;
    int32_t max_num_impacts_at_level0_{0};
    int32_t max_impact_num_bytes_at_level0_{0};
    int32_t max_num_impacts_at_level1_{0};
    int32_t max_impact_num_bytes_at_level1_{0};
};

/// Iterates over the postings of one term.
///
/// Port of `BlockPostingsEnum` (`core/codecs/lucene101/lucene101_postings_reader.rs`).
///
/// **Status:** fully ported: docs + freqs, both skip levels, positions,
/// offsets, payloads and impacts.
///
/// **Deviations:** the `I: IndexInput` parameter becomes
/// `std::unique_ptr<IndexInput>` inputs owned by a
/// `PostingDecodingUtil`; the reference implementation's `Identity` becomes the address of the
/// reader's docs input; the reference implementation's `BytesRef` for the serialized impacts /
/// payload becomes a `std::vector<uint8_t>` whose `size()` is the valid
/// length (`resize` is only called with the decoded length).
export class BlockPostingsEnum : public ImpactsEnum {
public:
    [[nodiscard]] static Result<std::unique_ptr<BlockPostingsEnum>>
    create(const FieldInfo &field_info, int32_t flags, bool needs_impacts, Lucene101PostingsReader &reader);

    /// Whether this instance can be reset for a term of the same shape.
    [[nodiscard]] bool can_reuse(const IndexInput *doc_input, const FieldInfo &field_info, int32_t flags,
                                 bool needs_impacts) const;

    /// Positions this enum on the term described by `term_state`.
    [[nodiscard]] VoidResult reset(const TermStateEnum &term_state, Lucene101PostingsReader &reader);

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(doc_freq_); }

    // ---- PostingsEnum ----

    [[nodiscard]] Result<int32_t> freq() override;
    [[nodiscard]] Result<int32_t> next_position() override;
    [[nodiscard]] Result<int32_t> start_offset() const override;
    [[nodiscard]] Result<int32_t> end_offset() const override;
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override;

    // ---- ImpactsSource / ImpactsEnum ----

    [[nodiscard]] VoidResult advance_shallow(int32_t target) override;
    [[nodiscard]] Result<std::unique_ptr<Impacts>> get_impacts() const override;

private:
    BlockPostingsEnum() = default;

    [[nodiscard]] VoidResult refill_full_block();
    [[nodiscard]] VoidResult refill_remainder();
    [[nodiscard]] VoidResult refill_docs();
    [[nodiscard]] VoidResult skip_level1_to(int32_t target);
    [[nodiscard]] VoidResult do_move_to_next_level0_block();
    [[nodiscard]] VoidResult move_to_next_level0_block();
    [[nodiscard]] VoidResult seek_pos_data(int64_t pos_fp, int32_t pos_upto, int64_t pay_fp, int32_t pay_upto);
    [[nodiscard]] VoidResult skip_level0_to(int32_t target);
    [[nodiscard]] VoidResult do_advance_shallow(int32_t target);
    [[nodiscard]] VoidResult skip_positions(int32_t freq);
    [[nodiscard]] VoidResult refill_last_position_block();
    [[nodiscard]] VoidResult refill_offsets_or_payloads();
    [[nodiscard]] VoidResult refill_positions();
    [[nodiscard]] VoidResult accumulate_pending_positions();
    [[nodiscard]] VoidResult accumulate_payload_and_offsets();

    [[nodiscard]] PostingDecodingUtil *doc_util();

    const IndexInput *doc_input_{nullptr};
    // The clones the decoding utils below read from; declared first so they
    // outlive the utils that reference them.
    std::unique_ptr<IndexInput> doc_in_clone_;
    std::unique_ptr<IndexInput> pos_in_clone_;
    std::unique_ptr<IndexInput> pay_in_clone_;
    std::optional<ForDeltaUtil> for_delta_util_;
    std::optional<PForUtil> pfor_util_;

    std::vector<int32_t> doc_buffer_;
    int32_t doc_{-1};

    int32_t level0_last_doc_id_{0};
    int64_t level0_doc_end_fp_{0};

    int32_t level1_last_doc_id_{0};
    int64_t level1_doc_end_fp_{0};
    int32_t level1_doc_count_upto_{0};

    int32_t doc_freq_{0};
    int64_t total_term_freq_{0};
    int32_t singleton_doc_id_{0};
    int32_t doc_count_left_{0};
    int32_t prev_doc_id_{0};

    size_t doc_buffer_size_{0};
    size_t doc_buffer_upto_{0};

    std::optional<PostingDecodingUtil> doc_in_util_;

    std::vector<int32_t> freq_buffer_;
    std::vector<int32_t> pos_delta_buffer_;
    std::vector<int32_t> payload_length_buffer_;
    std::vector<uint8_t> payload_bytes_;
    /// Snapshot of the payload a caller may still hold across a refill; the reference implementation
    /// keeps it in `payload: Option<BytesRef>`.
    std::vector<uint8_t> payload_value_;
    std::optional<std::pair<size_t, size_t>> payload_position_;
    std::vector<int32_t> offset_start_delta_buffer_;
    std::vector<int32_t> offset_length_buffer_;
    size_t payload_byte_upto_{0};
    size_t payload_length_{0};

    int32_t last_start_offset_{0};
    int32_t start_offset_{0};
    int32_t end_offset_{0};

    size_t pos_buffer_upto_{0};

    std::optional<PostingDecodingUtil> pos_in_util_;
    std::optional<PostingDecodingUtil> pay_in_util_;

    IndexOptions options_{IndexOptions::None};
    bool index_has_freq_{false};
    bool index_has_pos_{false};
    bool index_has_offsets_{false};
    bool index_has_payloads_{false};
    bool index_has_offsets_or_payloads_{false};

    int32_t flags_{0};
    bool needs_freq_{false};
    bool needs_pos_{false};
    bool needs_offsets_{false};
    bool needs_payloads_{false};
    bool needs_offsets_or_payloads_{false};
    bool needs_impacts_{false};
    bool needs_docs_and_freqs_only_{false};

    size_t freq_fp_{0};
    bool has_freq_fp_{false};
    int32_t position_{0};
    size_t pos_doc_buffer_upto_{0};
    int32_t pos_pending_count_{0};
    int64_t last_pos_block_fp_{0};

    int64_t level0_pos_end_fp_{0};
    int32_t level0_block_pos_upto_{0};
    int64_t level0_pay_end_fp_{0};
    int32_t level0_block_pay_upto_{0};
    std::vector<uint8_t> level0_serialized_impacts_;

    int64_t level1_pos_end_fp_{0};
    int32_t level1_block_pos_upto_{0};
    int64_t level1_pay_end_fp_{0};
    int32_t level1_block_pay_upto_{0};
    std::vector<uint8_t> level1_serialized_impacts_;

    bool needs_refilling_{false};

    int32_t max_num_impacts_at_level0_{0};
    int32_t max_num_impacts_at_level1_{0};
};

} // namespace lucene
