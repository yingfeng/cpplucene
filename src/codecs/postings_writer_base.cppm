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

export module lucene_core:postings_writer_base;

import :types;
import std;

import :block_term_state;
import :bytes_ref;
import :close;
import :data_output;
import :error;
import :field_infos;
import :fixed_bit_set;
import :index_output;
import :norms_producer;
import :postings_enum;
import :segment_info;
import :segment_write_state;
import :terms_enum;

namespace lucene {

/// Result of [`PostingsWriterBase::write_term`].
export struct WriteTermResult {
    /// The reused postings enum handed back to the caller; may be null.
    std::unique_ptr<PostingsEnum> postings;
    /// The term state, or `nullopt` when the term had no documents and the
    /// terms dict must skip it.
    std::optional<TermStateEnum> state;
};

/// Plugs into term dictionaries (for example `Lucene90BlockTreeTermsWriter`)
/// and handles writing postings.
///
/// Port of the `PostingsWriterBase` trait
/// (`core/codecs/postings_writer_base.rs`).
///
/// See also `PostingsReaderBase`.
///
/// **Deviations** (C++ mechanics only):
///  * The `NormsProducer` and `PostingsEnum` generic parameters of `write_term`
///    become a possibly-null `const NormsProducer *` and a
///    `std::unique_ptr<PostingsEnum>`; the `(Option<PE>, Option<TermStateEnum>)`
///    pair becomes [`WriteTermResult`].
///  * `Cow<TermStateEnum>` becomes a `const TermStateEnum &`.
export class PostingsWriterBase : public Closeable {
public:
    ~PostingsWriterBase() override = default;

    /// Called once after startup, before any terms have been added.
    /// Implementations typically write a header to `terms_out`.
    [[nodiscard]] virtual VoidResult init(IndexOutput &terms_out, const SegmentWriteState &state,
                                          const SegmentInfo &segment_info) = 0;

    /// Writes all postings for one term, pulling a `PostingsEnum` from
    /// `terms_enum`.
    ///
    /// The method must not re-position `terms_enum`: it is already positioned on
    /// the term that should be written, and it must set the bit in `docs_seen`
    /// for every doc id written.  When no docs were written it must report
    /// `nullopt` as the state, and the terms dict will skip the term.
    [[nodiscard]] virtual Result<WriteTermResult> write_term(const BytesRef &term, TermsEnum &terms_enum,
                                                             FixedBitSet &docs_seen, const NormsProducer *norms,
                                                             std::unique_ptr<PostingsEnum> postings_enum) = 0;

    /// Encodes the term metadata.
    ///
    /// `absolute` controls whether the current term is delta encoded according
    /// to the latest term: the encoded longs are usually file pointers, so each
    /// one increases when a new term is consumed.
    [[nodiscard]] virtual VoidResult encode_term(DataOutput &out, const FieldInfo &field_info,
                                                 const TermStateEnum &state, bool absolute) = 0;

    /// Sets the current field for writing.
    virtual void set_field(const FieldInfo &field_info) = 0;
};

} // namespace lucene
