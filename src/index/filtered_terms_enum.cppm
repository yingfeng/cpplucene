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

export module lucene_core:filtered_terms_enum;

import :types;
import std;

import :attribute_source;
import :block_term_state;
import :bytes_ref;
import :error;
import :impacts_enum;
import :postings_enum;
import :terms_enum;

namespace lucene {

/// Whether [`FilteredTermsEnumBase::accept`] accepts a term, and whether the
/// enumeration should seek afterwards or stop.
///
/// Port of `AcceptStatus` (`core/index/filtered_terms_enum.rs`).  The `*_and_seek`
/// values mean: after handling the current term, call
/// [`FilteredTermsEnumBase::next_seek_term`] and jump forward.
export enum class AcceptStatus : uint8_t {
    /// Accept the term and continue.
    kYes = 0,
    /// Accept the term, then seek to the term returned by `next_seek_term`.
    kYesAndSeek = 1,
    /// Reject the term and continue.
    kNo = 2,
    /// Reject the term, then seek to the term returned by `next_seek_term`.
    kNoAndSeek = 3,
    /// Reject the term and terminate the enumeration.
    kEnd = 4,
};

/// The filter part of a [`FilteredTermsEnum`].
///
/// Port of the `FilteredTermsEnumBase` trait (`core/index/filtered_terms_enum.rs`).
///
/// **Deviation:** `next_seek_term` takes the current term as a possibly-null
/// pointer (the reference implementation's `Option<&BytesRef>`) and returns an owned [`BytesRef`]:
/// The reference implementation's `Cow` is not needed, because the only borrowed case points into
/// the implementation itself and the caller copies it anyway.
export class FilteredTermsEnumBase {
public:
    virtual ~FilteredTermsEnumBase() = default;

    /// Whether `term` is accepted, rejected, or terminates the enumeration.
    [[nodiscard]] virtual Result<AcceptStatus> accept(const BytesRef &term, int64_t ord) = 0;

    /// The next term to seek to, or `nullopt` when the enumeration is over.
    ///
    /// The default implementation fails with `need_implemented`, which makes
    /// [`FilteredTermsEnum`] fall back to its initial seek term.
    [[nodiscard]] virtual Result<std::optional<BytesRef>> next_seek_term(const BytesRef *current);

    /// Whether [`accept`] needs the term ordinal.
    [[nodiscard]] virtual bool need_ord() const { return false; }
};

/// Enumerates a subset of all terms of a [`TermsEnum`].
///
/// Term enumerations are always ordered by [`BytesRef`]; each term of the
/// enumeration is greater than all that precede it.
///
/// **Note:** consumers cannot seek; the enum is forward only and the seek
/// methods fail with `unsupported_operation`.
///
/// Port of `FilteredTermsEnum` (`core/index/filtered_terms_enum.rs`).
///
/// **Deviations:** the reference implementation's `<T, F>` type parameters become two type-erased
/// pointers, and `FilteredTermsEnumHook::Default` becomes a null `hook` (which
/// is what [`unfiltered`] creates).
export class FilteredTermsEnum final : public TermsEnum {
public:
    /// A filtering enum; `start_with_seek` selects between seeking to the
    /// first accepted term and reading the wrapped enum sequentially.
    [[nodiscard]] static std::unique_ptr<FilteredTermsEnum> create(std::unique_ptr<TermsEnum> tenum,
                                                                   std::unique_ptr<FilteredTermsEnumBase> hook,
                                                                   bool start_with_seek);
    /// A pass-through enum that does not filter anything.
    [[nodiscard]] static std::unique_ptr<FilteredTermsEnum> unfiltered(std::unique_ptr<TermsEnum> tenum);

    /// Sets the term used when `next_seek_term` is not implemented; test hook.
    void set_initial_seek_term(BytesRef term) { initial_seek_term_ = std::move(term); }

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

    /// The wrapped enum.
    [[nodiscard]] TermsEnum &tenum() noexcept { return *tenum_; }
    /// Whether this enum filters anything.
    [[nodiscard]] bool is_filtered() const noexcept { return hook_ != nullptr; }

private:
    FilteredTermsEnum(std::unique_ptr<TermsEnum> tenum, std::unique_ptr<FilteredTermsEnumBase> hook, bool do_seek);

    /// `next_seek_term`, with the reference implementation's fallback to the initial seek term.
    [[nodiscard]] Result<std::optional<BytesRef>> hook_next_seek_term(const BytesRef *current);

    std::optional<BytesRef> initial_seek_term_{};
    bool do_seek_{true};
    bool has_actual_term_{false};
    std::unique_ptr<TermsEnum> tenum_;
    /// Null means "not filtered" (the reference implementation's `FilteredTermsEnumHook::Default`).
    std::unique_ptr<FilteredTermsEnumBase> hook_;
};

} // namespace lucene
