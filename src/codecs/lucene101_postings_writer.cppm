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

export module lucene_core:lucene101_postings_writer;

import :types;
import std;

import :bit_util;
import :block_term_state;
import :bytes_ref;
import :codec_util;
import :competitive_impact_accumulator;
import :data_output;
import :error;
import :field_infos;
import :for_delta_util;
import :growable_byte_array_data_output;
import :impact;
import :index_output;
import :lucene101_postings_format;
import :numeric_doc_values;
import :pfor_util;
import :postings_util;
import :push_postings_writer_base;
import :segment_info;
import :segment_write_state;

namespace lucene {

/// Writer for [`Lucene101PostingsFormat`].
///
/// Port of `Lucene101PostingsWriter`
/// (`core/codecs/lucene101/lucene101_postings_writer.rs`).
///
/// It implements the *push* API of [`PushPostingsWriterBaseAbstract`], which is
/// what the block tree terms writer drives.  The reference implementation also implements
/// `PostingsWriterBase` for it, but with `write_term` returning
/// `not_implemented` and `encode_term` returning `unreachable`, so those two
/// methods are not ported.
///
/// **Deviations** (C++ mechanics only):
///  * the output type parameter `O` disappears: every output is a
///    `std::unique_ptr<IndexOutput>`;
///  * `ByteBuffersDataOutput` (a resettable scratch buffer) is spelled
///    `GrowableByteArrayDataOutput`, which has the same `size()` / `copy_to()` /
///    `reset()` surface;
///  * `IOUtils::close_while_handling_exception` has no port yet, so a failed
///    construction or close simply closes whatever was opened and reports the
///    first error.
export class Lucene101PostingsWriter : public PushPostingsWriterBaseAbstract {
public:
    /// Opens the `.psm`, `.doc` (and optionally `.pos` / `.pay`) files and
    /// writes their headers.
    [[nodiscard]] static Result<std::unique_ptr<Lucene101PostingsWriter>> create(const SegmentWriteState &state,
                                                                                const SegmentInfo &segment_info);

    // ---- PushPostingsWriterBaseAbstract ----

    [[nodiscard]] VoidResult init(IndexOutput &terms_out, const SegmentWriteState &state,
                                  const SegmentInfo &segment_info) override;

    [[nodiscard]] Result<TermStateEnum> new_term_state() override;

    [[nodiscard]] VoidResult start_term(const FieldWriteOptions &options) override;

    [[nodiscard]] VoidResult finish_term(TermStateEnum &state, const FieldWriteOptions &options) override;

    [[nodiscard]] VoidResult start_doc(NumericDocValues *norms, int32_t doc_id, int32_t term_doc_freq,
                                       const FieldWriteOptions &options) override;

    [[nodiscard]] VoidResult add_position(int32_t position, const BytesRef *payload, int32_t start_offset,
                                          int32_t end_offset, const FieldWriteOptions &options) override;

    [[nodiscard]] VoidResult finish_doc() override;

    [[nodiscard]] VoidResult encode_term_with_option(DataOutput &out, const FieldInfo &field_info,
                                                     const TermStateEnum &state, bool absolute,
                                                     const FieldWriteOptions &options) override;

    void set_field(const FieldInfo &field_info) override;

    [[nodiscard]] VoidResult close() override;

    // ---- test / diagnostics accessors ----

    [[nodiscard]] bool is_closed() const noexcept { return closed_; }
    [[nodiscard]] int32_t doc_count() const noexcept { return doc_count_; }
    [[nodiscard]] int32_t max_num_impacts_at_level0() const noexcept { return max_num_impacts_at_level0_; }
    [[nodiscard]] int32_t max_impact_num_bytes_at_level0() const noexcept { return max_impact_num_bytes_at_level0_; }
    [[nodiscard]] int32_t max_num_impacts_at_level1() const noexcept { return max_num_impacts_at_level1_; }
    [[nodiscard]] int32_t max_impact_num_bytes_at_level1() const noexcept { return max_impact_num_bytes_at_level1_; }
    [[nodiscard]] IndexOutput *doc_output() noexcept { return doc_out_.get(); }

private:
    Lucene101PostingsWriter() = default;

    [[nodiscard]] Result<IndexOutput *> pos_out();
    [[nodiscard]] Result<IndexOutput *> pay_out();
    void close_quietly() noexcept;

    [[nodiscard]] VoidResult flush_doc_block(bool finish_term, const FieldWriteOptions &options);
    [[nodiscard]] VoidResult write_level1_skip_data(const FieldWriteOptions &options);

    std::unique_ptr<IndexOutput> meta_out_;
    std::unique_ptr<IndexOutput> doc_out_;
    std::unique_ptr<IndexOutput> pos_out_;
    std::unique_ptr<IndexOutput> pay_out_;
    bool closed_{false};

    IntBlockTermState last_state_;
    int64_t doc_start_fp_{0};
    int64_t pos_start_fp_{0};
    int64_t pay_start_fp_{0};

    std::vector<int32_t> doc_delta_buffer_;
    std::vector<int32_t> freq_buffer_;
    size_t doc_buffer_upto_{0};

    std::vector<int32_t> pos_delta_buffer_;
    std::vector<int32_t> payload_length_buffer_;
    std::vector<int32_t> offset_start_delta_buffer_;
    std::vector<int32_t> offset_length_buffer_;
    size_t pos_buffer_upto_{0};

    std::vector<uint8_t> payload_bytes_;
    size_t payload_byte_upto_{0};

    int32_t level0_last_doc_id_{0};
    int64_t level0_last_pos_fp_{0};
    int64_t level0_last_pay_fp_{0};

    int32_t level1_last_doc_id_{0};
    int64_t level1_last_pos_fp_{0};
    int64_t level1_last_pay_fp_{0};

    int32_t doc_id_{0};
    int32_t last_doc_id_{0};
    int32_t last_position_{0};
    int32_t last_start_offset_{0};
    int32_t doc_count_{0};

    PForUtil pfor_util_;
    ForDeltaUtil for_delta_util_;

    bool field_has_norms_{false};
    CompetitiveImpactAccumulator level0_freq_norm_accumulator_;
    CompetitiveImpactAccumulator level1_competitive_freq_norm_accumulator_;

    int32_t max_num_impacts_at_level0_{0};
    int32_t max_impact_num_bytes_at_level0_{0};
    int32_t max_num_impacts_at_level1_{0};
    int32_t max_impact_num_bytes_at_level1_{0};

    /// Scratch output used to prepend an encoded length (e.g. impacts).
    GrowableByteArrayDataOutput scratch_output_;
    /// Output for a single block, so skip data can be prepended once the block is
    /// encoded; the content is usually copied to [`level1_output_`].
    GrowableByteArrayDataOutput level0_output_;
    /// Output for groups of [`Lucene101PostingsFormat::LEVEL1_FACTOR`] blocks, so
    /// their skip data can be prepended; the content is copied to the doc file.
    GrowableByteArrayDataOutput level1_output_;
};

/// A vint that occupies 2 bytes when it needs 15 bits or less.
///
/// Variable length vints are slow when the byte count varies, so this special
/// layout helps in the (likely) case that 15 bits are enough.
[[nodiscard]] VoidResult write_vint15(DataOutput &out, int32_t v);

/// See [`write_vint15`].
[[nodiscard]] VoidResult write_vlong15(DataOutput &out, int64_t v);

/// Encodes `impacts` as `(freq_delta, norm_delta)` pairs, folding the common
/// "norm only grew by one" case into a single vint.
[[nodiscard]] VoidResult write_impacts(std::span<const Impact> impacts, DataOutput &out);

} // namespace lucene
