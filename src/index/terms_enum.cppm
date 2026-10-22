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

export module lucene_core:terms_enum;

import :types;
import std;

import :attribute_source;
import :block_term_state;
import :bytes_ref;
import :bytes_ref_iterator;
import :error;
import :impacts_enum;
import :postings_enum;

namespace lucene {

/// Result of [`TermsEnum::seek_ceil`].
export enum class SeekStatus : uint8_t {
    /// The term was not found and the end of iteration was hit.
    End = 0,
    /// The precise term was found.
    Found = 1,
    /// A different term was found after the requested one.
    NotFound = 2,
};

/// Result of a prepared seek (`PrepareSeekStatus` of rlucene).
export enum class PrepareSeekStatus : uint8_t {
    Pending = 0,
    Found = 1,
    NotFound = 2,
};

/// Iterator to seek ([`seek_ceil`], [`seek_exact`]) or step through
/// ([`BytesRefIterator::next`]) terms, and to obtain frequency information
/// ([`doc_freq`]), a `PostingsEnum` ([`postings`]) or an `ImpactsEnum`
/// ([`impacts`]) for the current term.
///
/// Term enumerations are always ordered by [`BytesRef`], and each term is
/// greater than the one before it.  A `TermsEnum` is unpositioned when first
/// obtained: `next()` or one of the `seek` methods must succeed first.
///
/// Port of the `TermsEnum` trait (`core/index/terms_enum.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * The `ByteSource` generic parameter of the seek methods becomes
///    `std::span<const uint8_t>`.
///  * The associated types `PostingsEnum` / `ImpactsEnum` become
///    `std::unique_ptr<PostingsEnum>` / `std::unique_ptr<ImpactsEnum>`; the
///    `Option<Self::PostingsEnum>` reuse parameter is a possibly-null
///    `std::unique_ptr`.
///  * The two `AttributeSource<'a>` GATs collapse into one mutable pointer:
///    C++ constness is a property of the reference, not of the type parameter.
///  * `term()` returns a borrowed pointer instead of an owned `Value`.
///  * `prepare_seek_exact` returns `true` for rlucene's `Some(())`.
export class TermsEnum : public BytesRefIterator {
public:
    /// The attribute source of this enum, or an error when it has none.
    [[nodiscard]] virtual Result<AttributeSource *> attributes() = 0;

    /// Mutable access to the attribute source (see the class note).
    [[nodiscard]] virtual Result<AttributeSource *> attributes_mut() = 0;

    /// Seeks to the exact term; `true` when it was found.
    [[nodiscard]] virtual Result<bool> seek_exact(std::span<const uint8_t> term) = 0;

    /// Two-phase [`seek_exact`]: the first phase typically prefetches the
    /// relevant bytes, the second one ([`get_prepare_seek_exact_status`])
    /// completes the seek.  `true` means a second phase is pending.
    [[nodiscard]] virtual Result<bool> prepare_seek_exact(std::span<const uint8_t> term) = 0;

    /// Executes a prepared seek and reports whether `target` exists.  `target`
    /// must be the term passed to [`prepare_seek_exact`].
    [[nodiscard]] virtual Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) = 0;

    /// Seeks to `term`, or to the next (ceiling) term.
    ///
    /// [`SeekStatus::End`] leaves the enum unpositioned.
    [[nodiscard]] virtual Result<SeekStatus> seek_ceil(std::span<const uint8_t> term) = 0;

    /// Seeks to the term with the given ordinal, as previously returned by
    /// [`ord`].
    [[nodiscard]] virtual VoidResult seek_exact_with_ord(int64_t ord) = 0;

    /// Expert: seeks a specific position by a [`TermStateEnum`] previously
    /// obtained from [`term_state`].  The state may only be used with the
    /// `TermsEnum` instance that produced it.
    [[nodiscard]] virtual VoidResult seek_exact_with_state(std::span<const uint8_t> term,
                                                           const TermStateEnum &state) = 0;

    /// The current term; must not be called when unpositioned.
    [[nodiscard]] virtual Result<const BytesRef *> term() const = 0;

    /// Ordinal position of the current term; optional for codecs.
    [[nodiscard]] virtual Result<int64_t> ord() const = 0;

    /// Number of documents containing the current term.
    [[nodiscard]] virtual Result<int32_t> doc_freq() = 0;

    /// Number of occurrences of the current term across all documents.
    [[nodiscard]] virtual Result<int64_t> total_term_freq() = 0;

    /// `PostingsEnum` for the current term; equivalent to
    /// `postings_with_flags(reuse, FREQS)`.
    [[nodiscard]] virtual Result<std::unique_ptr<PostingsEnum>> postings(std::unique_ptr<PostingsEnum> reuse) {
        return postings_with_flags(std::move(reuse), FREQS);
    }

    /// `PostingsEnum` for the current term, with control over which per-document
    /// values are required (see the `FREQS`/`POSITIONS`/`OFFSETS`/`PAYLOADS`
    /// flags of [`PostingsEnum`]).
    [[nodiscard]] virtual Result<std::unique_ptr<PostingsEnum>>
    postings_with_flags(std::unique_ptr<PostingsEnum> reuse, int32_t flags) = 0;

    /// an `ImpactsEnum` for the current term.
    [[nodiscard]] virtual Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t flags) = 0;

    /// The internal state needed to reposition the enum without re-seeking the
    /// term dictionary.
    [[nodiscard]] virtual Result<TermStateEnum> term_state() = 0;
};

/// A [`TermsEnum`] over nothing.  Port of `EmptyTermsEnum`
/// (`core/index/terms_enum.rs`).
///
/// **Deviation:** rlucene also has `EmptyTermsEnumTermsWrapper<T>`, which only
/// exists to name the wrapped `Terms`' postings type; type erasure makes it
/// redundant, so [`EmptyTermsEnum`] covers both.
export class EmptyTermsEnum : public TermsEnum {
public:
    [[nodiscard]] Result<const BytesRef *> next() override { return static_cast<const BytesRef *>(nullptr); }

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("EmptyTermsEnum has no attributes"));
    }

    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("EmptyTermsEnum has no attributes"));
    }

    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> term) override {
        auto status = seek_ceil(term);
        if (!status) {
            return std::unexpected(std::move(status.error()));
        }
        return status.value() == SeekStatus::Found;
    }

    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> /*term*/) override { return true; }

    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) override {
        return seek_exact(target);
    }

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> /*term*/) override {
        return SeekStatus::End;
    }

    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t /*ord*/) override { return {}; }

    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                   const TermStateEnum & /*state*/) override {
        return err(LuceneError::illegal_state("this method should never be called"));
    }

    [[nodiscard]] Result<const BytesRef *> term() const override {
        return err(LuceneError::illegal_state("this method should never be called"));
    }

    [[nodiscard]] Result<int64_t> ord() const override {
        return err(LuceneError::illegal_state("this method should never be called"));
    }

    [[nodiscard]] Result<int32_t> doc_freq() override {
        return err(LuceneError::illegal_state("this method should never be called"));
    }

    [[nodiscard]] Result<int64_t> total_term_freq() override {
        return err(LuceneError::illegal_state("this method should never be called"));
    }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>>
    postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/, int32_t /*flags*/) override {
        return err(LuceneError::illegal_state("this method should never be called"));
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::illegal_state("this method should never be called"));
    }

    [[nodiscard]] Result<TermStateEnum> term_state() override {
        return err(LuceneError::illegal_state("this method should never be called"));
    }
};

} // namespace lucene
