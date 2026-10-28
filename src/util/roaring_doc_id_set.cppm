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

export module lucene_core:roaring_doc_id_set;

import :accountable;
import :bits;
import :doc_id_set;
import :doc_id_set_iterator;
import :doc_id_set_impls;
import :error;
import :fixed_bit_set;
import :types;

import std;

namespace lucene {

/// A [`DocIdSet`] over one sorted array of 16 bit document ids, i.e. the
/// documents of a single roaring block.
///
/// Port of `util/roaring_doc_id_set.rs`'s `ShortArrayDocIdSet`.  The ids are
/// stored as unsigned 16 bit values so that the whole `0..=0xFFFF` block fits;
/// [`short_array_doc_id_index`] widens them back to `int32_t`.
export class ShortArrayDocIdSet final : public DocIdSet {
public:
    explicit ShortArrayDocIdSet(std::vector<uint16_t> doc_ids) : doc_ids_(std::move(doc_ids)) {}

    [[nodiscard]] Result<std::unique_ptr<DocIdSetIterator>> iterator() const override;

    [[nodiscard]] const std::vector<uint16_t> &doc_ids() const noexcept { return doc_ids_; }

private:
    std::vector<uint16_t> doc_ids_;
};

/// [`DocIdSetIterator`] over a [`ShortArrayDocIdSet`].
export class ShortArrayDISI final : public DocIdSetIterator {
public:
    explicit ShortArrayDISI(std::shared_ptr<const std::vector<uint16_t>> doc_ids)
        : doc_ids_(std::move(doc_ids)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

private:
    std::shared_ptr<const std::vector<uint16_t>> doc_ids_;
    std::optional<size_t> i_;
    int32_t doc_{-1};
};

/// [`DocIdSetIterator`] over a whole [`RoaringDocIdSet`]: it walks the blocks in
/// order and delegates to the iterator of the current block.
///
/// **Deviation:** the reference implementation stores a closed enum of the four
/// sub-iterator kinds; this port keeps one type erased sub-iterator, which is
/// equivalent because every operation on it goes through the
/// [`DocIdSetIterator`] interface.
export class RoaringDocIdSetIterator final : public DocIdSetIterator {
public:
    RoaringDocIdSetIterator(std::shared_ptr<const std::vector<std::shared_ptr<DocIdSet>>> doc_id_sets,
                            int64_t cardinality)
        : sets_(std::move(doc_id_sets)), cardinality_(cardinality) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return cardinality_; }

private:
    /// Opens the first document of the next non-empty block.
    [[nodiscard]] Result<int32_t> first_doc_from_next_block();

    std::shared_ptr<const std::vector<std::shared_ptr<DocIdSet>>> sets_;
    int64_t cardinality_;
    std::optional<size_t> block_;
    std::unique_ptr<DocIdSetIterator> sub_;
    int32_t doc_{-1};
};

/// [`DocIdSet`] implementation inspired by [roaringbitmap.org](http://roaringbitmap.org/).
///
/// The space is divided into blocks of `2^16` bits, and each block is encoded
/// independently.  In each block, if fewer than `2^12` bits are set, the
/// documents are stored in a plain array; if more than `2^16 - 2^12` bits are
/// set, the *inverse* of the set is stored in a plain array; otherwise a
/// [`FixedBitSet`] is used.
///
/// **Deviation:** `Accountable` is not ported (the port has no such trait), so
/// `ram_bytes_used` does not exist here; callers that need the footprint of a
/// cached set account for it themselves.
export class RoaringDocIdSet final : public DocIdSet, public Accountable {
public:
    /// Builds a [`RoaringDocIdSet`]; document ids must be added in strictly
    /// increasing order.
    class Builder {
    public:
        explicit Builder(size_t max_doc);

        /// Adds `doc_id`, which must be greater than the previously added one.
        [[nodiscard]] VoidResult add(int32_t doc_id);

        /// Adds every document of `disi`.
        [[nodiscard]] VoidResult add_disi(DocIdSetIterator &disi);

        /// Encodes the current block and returns the set.
        [[nodiscard]] Result<std::shared_ptr<RoaringDocIdSet>> build();

    private:
        [[nodiscard]] VoidResult flush();

        size_t max_doc_;
        std::vector<std::shared_ptr<DocIdSet>> sets_;
        size_t cardinality_{0};
        int32_t last_doc_id_{-1};
        std::optional<size_t> current_block_;
        size_t current_block_cardinality_{0};
        std::vector<uint16_t> buffer_;
        std::optional<FixedBitSet> dense_buffer_;
    };

    [[nodiscard]] Result<std::unique_ptr<DocIdSetIterator>> iterator() const override;

    /// Number of documents in the set.
    [[nodiscard]] size_t cardinality() const noexcept { return cardinality_; }

    /// Number of blocks the set spans (`ceil(max_doc / 2^16)`).
    [[nodiscard]] size_t num_blocks() const noexcept { return sets_->size(); }

    /// Retained bytes: every block's encoding plus the block index.
    [[nodiscard]] int64_t ram_bytes_used() const override;

private:
    RoaringDocIdSet(std::shared_ptr<const std::vector<std::shared_ptr<DocIdSet>>> sets, size_t cardinality)
        : sets_(std::move(sets)), cardinality_(cardinality) {}

    std::shared_ptr<const std::vector<std::shared_ptr<DocIdSet>>> sets_;
    size_t cardinality_{0};
};

} // namespace lucene
