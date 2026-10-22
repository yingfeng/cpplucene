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

export module lucene_core:multi_fields;

import :types;
import std;

import :attribute_source;
import :block_term_state;
import :bytes_ref;
import :compiled_automaton;
import :error;
import :fields;
import :impacts_enum;
import :postings_enum;
import :terms;
import :terms_enum;

namespace lucene {

/// One sub-reader of a [`MultiFields`]: where its documents start in the
/// composite reader's doc-id space and which reader it is.
///
/// Port of `ReaderSlice`.
export class ReaderSlice {
public:
    ReaderSlice(int32_t start, int32_t length, size_t reader_index)
            : start_(start), length_(length), reader_index_(reader_index) {}

    [[nodiscard]] int32_t start() const noexcept { return start_; }
    [[nodiscard]] int32_t length() const noexcept { return length_; }
    [[nodiscard]] size_t reader_index() const noexcept { return reader_index_; }

private:
    int32_t start_;
    int32_t length_;
    size_t reader_index_;
};

/// Iterates over the sorted union of the field names of several [`Fields`].
class MergedFieldNameIterator final : public FieldNameIterator {
public:
    explicit MergedFieldNameIterator(std::vector<std::string> names) : names_(std::move(names)) {}

    [[nodiscard]] Result<const std::string *> next() override {
        if (position_ >= names_.size()) {
            return static_cast<const std::string *>(nullptr);
        }
        return &names_[position_++];
    }

private:
    std::vector<std::string> names_;
    size_t position_{0};
};

/// The postings of one term across several sub-readers.
///
/// Port of `MultiPostingsEnum`.  The subs are ordered by reader index, so their
/// (offset) doc ids come out in increasing order.
export class MultiPostingsEnum final : public PostingsEnum {
public:
    /// One sub-reader's postings for the current term.
    struct Sub {
        std::unique_ptr<PostingsEnum> postings;
        ReaderSlice slice;
    };

    explicit MultiPostingsEnum(std::vector<Sub> subs);

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override;

    [[nodiscard]] Result<int32_t> freq() override;
    [[nodiscard]] Result<int32_t> next_position() override;
    [[nodiscard]] Result<int32_t> start_offset() const override;
    [[nodiscard]] Result<int32_t> end_offset() const override;
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override;

    [[nodiscard]] std::vector<Sub> &subs() noexcept { return subs_; }
    [[nodiscard]] const std::vector<Sub> &subs() const noexcept { return subs_; }

private:
    [[nodiscard]] PostingsEnum &current_postings();
    [[nodiscard]] const PostingsEnum &current_postings() const;

    std::vector<Sub> subs_;
    std::optional<size_t> upto_;
    std::optional<size_t> current_;
    int32_t current_base_{0};
    int32_t doc_{-1};
};

/// Exposes the term index of several sub-readers as one.
///
/// Port of `MultiFields`.  Unlike the reference implementation this port does not cache the
/// `MultiTerms` it hands out (its `Fields::terms` returns an owning pointer, so
/// caching would need an extra indirection); each call rebuilds the merge.
export class MultiFields final : public Fields {
public:
    /// One sub-reader's `Fields`, with its slice.
    struct Sub {
        Fields *fields;
        ReaderSlice slice;
    };

    explicit MultiFields(std::vector<Sub> subs) : subs_(std::move(subs)) {}

    [[nodiscard]] Result<std::unique_ptr<FieldNameIterator>> iterator() const override;
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) const override;
    /// Always `-1`: the union of several readers has no term count statistic.
    [[nodiscard]] Result<int32_t> size() const override { return -1; }

private:
    std::vector<Sub> subs_;
};

/// Exposes the terms of one field of several sub-readers as one.
///
/// Port of `MultiTerms`.  The feature flags are the intersection of the subs'
/// flags, except payloads which only needs one sub to have them (and requires
/// positions).
export class MultiTerms final : public Terms {
public:
    /// One sub-reader's `Terms`, with its slice.
    struct Sub {
        std::unique_ptr<Terms> terms;
        ReaderSlice slice;
    };

    [[nodiscard]] static Result<std::unique_ptr<MultiTerms>> create(std::vector<Sub> subs);

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override;
    [[nodiscard]] Result<int64_t> size() const override { return -1; }
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override;
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override;
    [[nodiscard]] Result<int32_t> doc_count() const override;
    [[nodiscard]] bool has_freqs() const override { return has_freqs_; }
    [[nodiscard]] bool has_offsets() const override { return has_offsets_; }
    [[nodiscard]] bool has_positions() const override { return has_positions_; }
    [[nodiscard]] bool has_payloads() const override { return has_payloads_; }

    [[nodiscard]] const std::vector<Sub> &subs() const noexcept { return subs_; }

private:
    MultiTerms(std::vector<Sub> subs, bool has_freqs, bool has_offsets, bool has_positions, bool has_payloads);

    std::vector<Sub> subs_;
    bool has_freqs_;
    bool has_offsets_;
    bool has_positions_;
    bool has_payloads_;
};

/// Merges the term dictionaries of several sub-readers.
///
/// Port of `MultiTermsEnum`.  **Deviations:**
///  * the merge keeps the active subs in a plain vector and rescans it for the
///    smallest term (the reference implementation uses a `TopTermsPriorityQueue`); the number of
///    sub-readers is small, so the quadratic factor does not matter and the code
///    stays obvious;
///  * the seek API is not supported (only forward iteration, which is all a
///    merge needs);
///  * impacts and term states are not supported;
///  * `postings` always allocates instead of reusing a previous enumeration.
export class MultiTermsEnum final : public TermsEnum {
public:
    /// One sub-reader's term enumeration, with its slice.
    struct Sub {
        std::unique_ptr<TermsEnum> terms_enum;
        ReaderSlice slice;
    };

    explicit MultiTermsEnum(std::vector<Sub> subs) : subs_(std::move(subs)) {}

    [[nodiscard]] Result<const BytesRef *> next() override;
    [[nodiscard]] Result<const BytesRef *> term() const override;
    [[nodiscard]] Result<int64_t> ord() const override { return -1; }
    [[nodiscard]] Result<int32_t> doc_freq() override;
    [[nodiscard]] Result<int64_t> total_term_freq() override;

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(
            std::unique_ptr<PostingsEnum> /*reuse*/, int32_t flags) override;
    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override;

    [[nodiscard]] Result<AttributeSource *> attributes() override;
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override { return attributes(); }
    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> /*term*/) override;
    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> /*term*/) override;
    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> /*target*/) override;
    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> /*term*/) override;
    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t /*ord*/) override;
    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                   const TermStateEnum & /*state*/) override;
    [[nodiscard]] Result<TermStateEnum> term_state() override;

private:
    /// The term the sub is positioned on, or `nullptr` when it is exhausted.
    [[nodiscard]] Result<const BytesRef *> term_of(size_t index) const;

    std::vector<Sub> subs_;
    /// Indices of the subs that are positioned on a term.
    std::vector<size_t> active_;
    /// Indices of the subs that are positioned on the current term.
    std::vector<size_t> top_;
    /// Owns the bytes of `current_term_`: a sub's term() is only valid until the
    /// next call, so the merge keeps its own copy.
    std::vector<uint8_t> current_term_bytes_;
    BytesRef current_term_;
    bool initialized_{false};
    bool positioned_{false};
};

} // namespace lucene
