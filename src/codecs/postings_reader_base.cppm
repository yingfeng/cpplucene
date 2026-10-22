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

export module lucene_core:postings_reader_base;

import :types;
import std;

import :error;
import :block_term_state;
import :data_input;
import :field_infos;
import :impacts_enum;
import :index_input;
import :postings_enum;
import :segment_info;
import :segment_read_state;

namespace lucene {

/// The core terms dictionaries (`BlockTermsReader`, `BlockTreeTermsReader`)
/// interact with a single instance of this class to manage creation of
/// `PostingsEnum` and `ImpactsEnum` instances.  It provides an `IndexInput`
/// (`terms_in`) where the postings reader may read any previously stored data
/// that its corresponding `PostingsWriterBase` wrote at indexing time.
///
/// Port of the `PostingsReaderBase` trait (`core/codecs/postings_reader_base.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * rlucene's associated types `PostingsEnum` / `ImpactsEnum` become
///    `std::unique_ptr<PostingsEnum>` / `std::unique_ptr<ImpactsEnum>`; the
///    `Option<Self::PostingsEnum>` reuse parameter becomes a possibly-null
///    `std::unique_ptr`.
///  * `Display` becomes [`to_string`] and the `CloseableRef` supertrait becomes
///    the defaulted [`close`], because the close subsystem is not ported yet.
export class PostingsReaderBase {
public:
    virtual ~PostingsReaderBase() = default;

    /// Performs any initialization, such as reading and verifying the header
    /// from the provided terms dictionary input.
    [[nodiscard]] virtual VoidResult init(IndexInput &terms_in, const SegmentReadState &state,
                                          const SegmentInfo &segment_info) = 0;

    /// A newly created, empty term state.
    [[nodiscard]] virtual Result<TermStateEnum> new_term_state() const = 0;

    /// Decodes the metadata of the next term.
    ///
    /// See also `PostingsWriterBase::encode_term`.
    [[nodiscard]] virtual VoidResult decode_term(DataInput &input, const FieldInfo &field_info, TermStateEnum &state,
                                                 bool absolute) const = 0;

    /// Must fully consume `state`, since after this call the term state may be
    /// reused.
    [[nodiscard]] virtual Result<std::unique_ptr<PostingsEnum>>
    postings(const FieldInfo &field_info, const TermStateEnum &state, std::unique_ptr<PostingsEnum> reuse,
             int32_t flags) const = 0;

    /// An `ImpactsEnum` that computes impacts for the term described by `state`.
    ///
    /// See also [`postings`].
    [[nodiscard]] virtual Result<std::unique_ptr<ImpactsEnum>>
    impacts(const FieldInfo &field_info, const TermStateEnum &state, int32_t flags) const = 0;

    /// Checks consistency of this reader.
    ///
    /// NOTE: this may be costly in terms of I/O, e.g. it may involve computing a
    /// checksum value against large data files.
    [[nodiscard]] virtual VoidResult check_integrity() const = 0;

    /// Releases the resources held by this reader; idempotent.
    [[nodiscard]] virtual VoidResult close() { return {}; }

    /// Human readable description (`Display` in rlucene).
    [[nodiscard]] virtual std::string to_string() const { return "PostingsReaderBase"; }
};

} // namespace lucene
