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

export module lucene_core:terms;

import :types;
import std;

import :bytes_ref;
import :bytes_ref_iterator;
import :compiled_automaton;
import :error;
import :terms_enum;

namespace lucene {

/// Base term statistics and access for one field of one segment.
///
/// Port of the `Terms` trait (`core/index/terms.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the associated `TermsEnum` / `IntersectIter` types collapse into
///    `std::unique_ptr<TermsEnum>`: type erasure replaces the reference implementation's GATs;
///  * `get_min` / `get_max` return an owned [`BytesRef`] instead of a `Cow`
///    (the iterator's term is copied, which is what `Cow::Owned` did anyway);
///  * the reference implementation's `CompiledAutomaton::get_terms_enum` becomes
///    [`Terms::terms_enum_for`], because `:compiled_automaton` importing
///    `:terms` would be a module cycle.
export class Terms {
public:
    virtual ~Terms() = default;

    /// An iterator that steps through all terms of this field; never null.
    [[nodiscard]] virtual Result<std::unique_ptr<TermsEnum>> iterator() const = 0;

    /// Number of terms for this field, or `-1` when the codec does not store
    /// it.  Deleted documents are not taken into account.
    [[nodiscard]] virtual Result<int64_t> size() const = 0;

    /// Sum of `TermsEnum::total_term_freq` over all terms.
    [[nodiscard]] virtual Result<int64_t> sum_total_term_freq() const = 0;

    /// Sum of `TermsEnum::doc_freq` over all terms.
    [[nodiscard]] virtual Result<int64_t> sum_doc_freq() const = 0;

    /// Number of documents that have at least one term for this field.
    [[nodiscard]] virtual Result<int32_t> doc_count() const = 0;

    /// Whether documents store per-document term frequencies.
    [[nodiscard]] virtual bool has_freqs() const = 0;
    /// Whether documents store offsets.
    [[nodiscard]] virtual bool has_offsets() const = 0;
    /// Whether documents store positions.
    [[nodiscard]] virtual bool has_positions() const = 0;
    /// Whether documents store payloads.
    [[nodiscard]] virtual bool has_payloads() const = 0;

    /// The smallest term (lexicographic order), or `std::nullopt` when the field
    /// has no terms.
    [[nodiscard]] virtual Result<std::optional<BytesRef>> get_min() const;

    /// The largest term (lexicographic order), or `std::nullopt` when the field
    /// has no terms.
    [[nodiscard]] virtual Result<std::optional<BytesRef>> get_max() const;

    /// Human readable debug statistics.
    [[nodiscard]] virtual Result<std::string> get_stats() const;

    /// The reference implementation's `default_intersect`: an enum over the terms accepted by
    /// `compiled`, built by filtering [`iterator`] with an automaton.
    ///
    /// `start_term` is the term to start enumerating from, when non-null.  Note
    /// that the reference implementation's `AutomatonTermsEnum` treats it as **exclusive**: a term
    /// equal to `start_term` is never returned, even when the automaton accepts
    /// it (compare `test_intersect` in the reference implementation, where `start_term = "aaa"` and
    /// the terms `aaa, bbb, ccc` yield `bbb` first).  This port keeps that
    /// behavior.
    ///
    /// Codecs with a more efficient intersection (the block tree walks its
    /// prefix trie) override this.
    [[nodiscard]] virtual Result<std::unique_ptr<TermsEnum>> intersect(const CompiledAutomaton &compiled,
                                                                      const BytesRef *start_term) const;

    /// The reference implementation's `CompiledAutomaton::get_terms_enum`: an enum over the terms
    /// accepted by `compiled`, taking the specialized form for the simple cases
    /// (nothing, everything, a single term).
    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> terms_enum_for(const CompiledAutomaton &compiled) const;
};

/// [`Terms`] over nothing.  Port of `EmptyTerms` (`core/index/terms.rs`).
export class EmptyTerms : public Terms {
public:
    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override;
    [[nodiscard]] Result<int64_t> size() const override { return 0; }
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override { return 0; }
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override { return 0; }
    [[nodiscard]] Result<int32_t> doc_count() const override { return 0; }
    [[nodiscard]] bool has_freqs() const override { return false; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }
    [[nodiscard]] Result<std::optional<BytesRef>> get_min() const override { return std::optional<BytesRef>{}; }
    [[nodiscard]] Result<std::optional<BytesRef>> get_max() const override { return std::optional<BytesRef>{}; }
};

} // namespace lucene
