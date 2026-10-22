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

export module lucene_core:push_postings_writer_base;

import :types;
import std;

import :block_term_state;
import :bytes_ref;
import :close;
import :data_output;
import :doc_id_set_iterator;
import :error;
import :field_infos;
import :fixed_bit_set;
import :index_output;
import :index_types;
import :norms_producer;
import :numeric_doc_values;
import :postings_enum;
import :postings_writer_base;
import :segment_info;
import :segment_write_state;
import :terms_enum;

namespace lucene {

/// What the current field writes; computed from the field's [`FieldInfo`].
export struct FieldWriteOptions {
    /// `true` when the current field writes freqs.
    bool write_freqs{false};
    /// `true` when the current field writes positions.
    bool write_positions{false};
    /// `true` when the current field writes payloads.
    bool write_payloads{false};
    /// `true` when the current field writes offsets.
    bool write_offsets{false};
};

/// The "sub" half of [`PushPostingsWriterBase`]: the codec specific push API.
///
/// Port of the `PushPostingsWriterBaseAbstract` trait
/// (`core/codecs/push_postings_writer_base.rs`).
///
/// **Deviation:** the reference implementation bounds the wrapper with
/// `S: PushPostingsWriterBaseAbstract + PostingsWriterBase` and delegates
/// `init` to the sub writer through the second bound.  Inheriting
/// [`PostingsWriterBase`] here would force every codec specific writer to also
/// implement the wrapper's `write_term`, so this interface declares `init`
/// itself and the reference implementation's `PostingsWriterBase::init` is what the wrapper's `init`
/// forwards to.
export class PushPostingsWriterBaseAbstract : public Closeable {
public:
    ~PushPostingsWriterBaseAbstract() override = default;

    /// Called once after startup, before any terms have been added.
    [[nodiscard]] virtual VoidResult init(IndexOutput &terms_out, const SegmentWriteState &state,
                                          const SegmentInfo &segment_info) = 0;

    /// A newly created, empty term state.
    [[nodiscard]] virtual Result<TermStateEnum> new_term_state() = 0;

    /// Starts a new term.  A matching call to [`finish_term`] is done only when
    /// the term has at least one document.
    [[nodiscard]] virtual VoidResult start_term(const FieldWriteOptions &options) = 0;

    /// Finishes the current term.  `state` carries the term's summary statistics
    /// and receives the codec metadata.
    [[nodiscard]] virtual VoidResult finish_term(TermStateEnum &state, const FieldWriteOptions &options) = 0;

    /// Adds a new doc in this term.  `freq` is `-1` when term frequencies are
    /// omitted for the field, and `norms` is null when the field has no norms.
    [[nodiscard]] virtual VoidResult start_doc(NumericDocValues *norms, int32_t doc_id, int32_t freq,
                                               const FieldWriteOptions &options) = 0;

    /// Adds a new position, payload and offset pair.
    ///
    /// A null payload means "no payload", as does a present payload with zero
    /// length.  The method must fully consume the payload, since the caller may
    /// reuse the buffer.  `start_offset` / `end_offset` are `-1` when offsets are
    /// not indexed.
    [[nodiscard]] virtual VoidResult add_position(int32_t position, const BytesRef *payload, int32_t start_offset,
                                                  int32_t end_offset, const FieldWriteOptions &options) = 0;

    /// Called when the positions and payloads of the current doc are complete.
    [[nodiscard]] virtual VoidResult finish_doc() = 0;

    /// Encodes the term metadata, honouring the field's write options.
    ///
    /// The reference implementation names this `encode_term_with_option` to tell it apart from
    /// `PostingsWriterBase::encode_term`, which the wrapper delegates here.
    [[nodiscard]] virtual VoidResult encode_term_with_option(DataOutput &out, const FieldInfo &field_info,
                                                             const TermStateEnum &state, bool absolute,
                                                             const FieldWriteOptions &options) = 0;

    /// Sets the current field for writing (the reference implementation's
    /// `PushPostingsWriterBaseAbstract::set_field`).
    virtual void set_field(const FieldInfo &field_info) = 0;
};

/// Extension of [`PostingsWriterBase`] that adds a push API for writing each
/// element of the postings.  This API is somewhat analogous to an XML SAX API,
/// while [`PostingsWriterBase`] is more like an XML DOM API.
///
/// Port of `PushPostingsWriterBase<S>` (`core/codecs/push_postings_writer_base.rs`).
///
/// **Deviation:** the `S` type parameter becomes a
/// `std::unique_ptr<PushPostingsWriterBaseAbstract>`, so the wrapper is a plain
/// class instead of a template.
export class PushPostingsWriterBase : public PostingsWriterBase {
public:
    explicit PushPostingsWriterBase(std::unique_ptr<PushPostingsWriterBaseAbstract> sub);

    [[nodiscard]] VoidResult init(IndexOutput &terms_out, const SegmentWriteState &state,
                                  const SegmentInfo &segment_info) override;

    [[nodiscard]] Result<WriteTermResult> write_term(const BytesRef &term, TermsEnum &terms_enum,
                                                     FixedBitSet &docs_seen, const NormsProducer *norms,
                                                     std::unique_ptr<PostingsEnum> postings_enum) override;

    [[nodiscard]] VoidResult encode_term(DataOutput &out, const FieldInfo &field_info, const TermStateEnum &state,
                                         bool absolute) override;

    void set_field(const FieldInfo &field_info) override;

    [[nodiscard]] VoidResult close() override;

    /// The write options of the current field (the reference implementation's crate-visible field).
    [[nodiscard]] const FieldWriteOptions &options() const noexcept { return options_; }

    /// The flags passed to `TermsEnum::postings_with_flags` for the current
    /// field (the reference implementation's `enum_flags`).
    [[nodiscard]] int32_t enum_flags() const noexcept { return enum_flags_; }

    /// The field currently being written, or `nullptr` before `set_field`.
    [[nodiscard]] const FieldInfo *field_info() const noexcept { return field_info_.has_value() ? &field_info_.value() : nullptr; }

private:
    int32_t enum_flags_{0};
    std::optional<FieldInfo> field_info_;
    IndexOptions index_options_{IndexOptions::None};
    FieldWriteOptions options_;
    std::unique_ptr<PushPostingsWriterBaseAbstract> sub_;
};

} // namespace lucene
