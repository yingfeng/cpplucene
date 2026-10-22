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

export module lucene_core:terms_hash_per_field;

import :types;
import std;

import :byte_block_pool;
import :byte_slice_pool;
import :byte_slice_reader;
import :bytes_ref;
import :bytes_ref_hash;
import :error;
import :index_types;
import :int_block_pool;
import :parallel_postings_array;

namespace lucene {

/// Number of term ids a fresh [`TermsHashPerField`] hash starts with.
export inline constexpr size_t HASH_INIT_SIZE = 4;

/// Creates the postings array of one field.
///
/// Port of the reference implementation's `TermsHashPerFieldType` enum dispatch
/// (`terms_hash_per_field.rs`).  The concrete implementation lives next to the
/// flavour it builds (`FreqProxTermsHashType` in
/// `freq_prox_terms_writer_per_field`), which keeps this module free of a cycle
/// with the writers.
export class TermsHashPerFieldType {
public:
    virtual ~TermsHashPerFieldType() = default;

    /// A postings array that can hold `size` term ids.
    [[nodiscard]] virtual std::unique_ptr<PostingsArray> new_per_field(size_t size) const = 0;
};

/// Owns the postings array of one per-field terms hash.
///
/// Port of `PostingsArrayWrapper` (`terms_hash_per_field.rs`).
export class PostingsArrayHolder {
public:
    explicit PostingsArrayHolder(std::unique_ptr<TermsHashPerFieldType> type) : type_(std::move(type)) {}

    PostingsArrayHolder(const PostingsArrayHolder &) = delete;
    PostingsArrayHolder &operator=(const PostingsArrayHolder &) = delete;
    PostingsArrayHolder(PostingsArrayHolder &&) noexcept = default;
    PostingsArrayHolder &operator=(PostingsArrayHolder &&) noexcept = default;
    ~PostingsArrayHolder() = default;

    [[nodiscard]] const PostingsArray *postings_array() const noexcept { return postings_array_.get(); }
    [[nodiscard]] PostingsArray *postings_array_mut() noexcept { return postings_array_.get(); }
    [[nodiscard]] const TermsHashPerFieldType &type() const noexcept { return *type_; }

    void set(std::unique_ptr<PostingsArray> array) noexcept { postings_array_ = std::move(array); }
    [[nodiscard]] std::unique_ptr<PostingsArray> take() noexcept { return std::move(postings_array_); }

private:
    std::unique_ptr<TermsHashPerFieldType> type_;
    std::unique_ptr<PostingsArray> postings_array_;
};

/// The [`BytesStartArray`] that keeps the postings array of a field.
///
/// Port of `PostingsBytesStartArray` (`terms_hash_per_field.rs`).  `add` uses
/// it to turn term ids into `text_starts` offsets, and
/// [`TermsHashPerField`] uses the embedded holder to reach the other arrays.
///
/// **Deviation:** the RAM counter is gone (no `Accountable` in this port), so
/// `grow`/`clear` only resize and release the arrays.
export class PostingsBytesStartArray final : public BytesStartArray {
public:
    explicit PostingsBytesStartArray(std::unique_ptr<TermsHashPerFieldType> type)
            : holder_(std::move(type)) {}

    [[nodiscard]] VoidResult init() override;
    [[nodiscard]] VoidResult grow() override;
    void clear() override;
    [[nodiscard]] Result<int32_t> get_value(size_t index) const override;
    [[nodiscard]] VoidResult set_value(size_t index, int32_t value) override;
    [[nodiscard]] Result<size_t> len() const override;
    [[nodiscard]] bool need_init() const override { return holder_.postings_array() == nullptr; }

    [[nodiscard]] PostingsArrayHolder &holder() noexcept { return holder_; }
    [[nodiscard]] const PostingsArrayHolder &holder() const noexcept { return holder_; }

private:
    [[nodiscard]] Result<const PostingsArray *> postings_array() const;
    [[nodiscard]] Result<PostingsArray *> postings_array_mut();

    PostingsArrayHolder holder_;
};

/// Stores per-term streams of information whose size is unknown up front.
///
/// Port of `TermsHashPerField` (`terms_hash_per_field.rs`).  Each stream is a
/// `ByteSlicePool` chain; the terms are deduplicated in a [`BytesRefHash`]
/// whose `bytes_start` table is a [`PostingsBytesStartArray`] (so term ids index
/// straight into the postings arrays).
///
/// The subclasses (`FreqProxTermsWriterPerField`, and later the term-vectors
/// writer) provide `new_term`/`add_term`; the reference implementation expresses that with the
/// `TermsHashPerFieldBase` trait, which C++ spells as virtual methods on those
/// subclasses (they are not needed here, so this class stays concrete).
export class TermsHashPerField {
public:
    /// `stream_count` streams per term: 1 for doc(+freq), 2 for prox+offset.
    TermsHashPerField(int32_t stream_count, std::unique_ptr<TermsHashPerFieldType> type, IndexOptions index_options);

    TermsHashPerField(const TermsHashPerField &) = delete;
    TermsHashPerField &operator=(const TermsHashPerField &) = delete;
    TermsHashPerField(TermsHashPerField &&) noexcept = default;
    TermsHashPerField &operator=(TermsHashPerField &&) noexcept = default;
    ~TermsHashPerField() = default;

    /// Positions `reader` at the start of `stream` of `term_id`.
    [[nodiscard]] VoidResult init_reader(ByteSliceReader &reader, size_t term_id, int32_t stream,
                                         const IntBlockPool &int_pool) const;

    /// Collapses the hash and sorts the term ids; call once per flush.
    [[nodiscard]] VoidResult sort_terms(const ByteBlockPool &byte_pool);

    /// Sorted term ids; [`sort_terms`] must have been called.
    [[nodiscard]] const std::vector<int32_t> &get_sorted_term_ids() const noexcept { return bytes_hash_.ids(); }

    [[nodiscard]] VoidResult write_byte(int32_t stream, uint8_t b, IntBlockPool &int_pool, ByteBlockPool &byte_pool) const;

    [[nodiscard]] VoidResult write_bytes(int32_t stream, std::span<const uint8_t> b, size_t offset, size_t len,
                                         IntBlockPool &int_pool, ByteBlockPool &byte_pool) const;

    [[nodiscard]] VoidResult write_vint(int32_t stream, int32_t i, IntBlockPool &int_pool,
                                        ByteBlockPool &byte_pool) const;

    [[nodiscard]] size_t get_num_terms() const noexcept { return bytes_hash_.size(); }

    [[nodiscard]] const PostingsArray *postings_array() const noexcept {
        return holder_ != nullptr ? holder_->postings_array() : nullptr;
    }
    [[nodiscard]] PostingsArray *postings_array_mut() noexcept {
        return holder_ != nullptr ? holder_->postings_array_mut() : nullptr;
    }

    /// Empties the hash but keeps the pools (the writer reuses them).
    void reset(ByteBlockPool &byte_pool);

    /// Re-allocates the `bytes_start` table after [`reset`] / [`close`].
    [[nodiscard]] VoidResult reinit_hash();

    /// Allocates the slices of a term that is seen for the first time.
    [[nodiscard]] VoidResult init_stream_slices(size_t term_id, int32_t doc_id, IntBlockPool &int_pool,
                                                ByteBlockPool &byte_pool);

    /// Debug-only monotonicity check of the document ids.
    bool assert_doc_id(int32_t doc_id);

    /// Points the write cursor at the streams of an existing term; returns its
    /// id (the reference implementation returns it as `usize`).
    [[nodiscard]] Result<size_t> position_stream_slice(int32_t term_id, int32_t doc_id);

    [[nodiscard]] BytesRefHash &bytes_hash() noexcept { return bytes_hash_; }
    [[nodiscard]] const BytesRefHash &bytes_hash() const noexcept { return bytes_hash_; }
    [[nodiscard]] IndexOptions index_options() const noexcept { return index_options_; }
    [[nodiscard]] int32_t stream_count() const noexcept { return stream_count_; }

private:
    /// Stateless helper: `write_byte`/`write_bytes` only forward the pool they
    /// are given, so the sorter stays usable from a const method.
    mutable ByteSlicePool slice_pool_;
    /// Index of the int block that holds the stream addresses.
    size_t term_stream_address_buffer_index_{0};
    /// Offset inside that block of the first stream of the current term.
    int32_t stream_address_offset_{0};
    int32_t stream_count_{0};
    BytesRefHash bytes_hash_;
    /// Borrowed from `bytes_hash_`'s `PostingsBytesStartArray`.
    PostingsArrayHolder *holder_{nullptr};
    IndexOptions index_options_{IndexOptions::None};
    /// Debug-only: whether [`sort_terms`] has been called.
    bool is_sorted_{false};
    /// Debug-only: id of the last document written.
    int32_t last_doc_id_{0};
};

} // namespace lucene
