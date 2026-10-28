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

export module lucene_core:doc_id_set_impls;

import :accountable;
import :types;
import std;

import :bits;
import :doc_id_set;
import :doc_id_set_iterator;
import :error;
import :fixed_bit_set;

namespace lucene {

// ---------------------------------------------------------------------------
// IntArrayDocIdSet
// ---------------------------------------------------------------------------

/// A [`DocIdSet`] over a sorted `int32_t` array.
///
/// Port of `util/int_array_doc_id_set.rs`.  `docs[length]` must hold
/// [`NO_MORE_DOCS`], which is what lets the iterator step past the last
/// document without a bounds check.
export class IntArrayDocIdSet final : public DocIdSet, public Accountable {
public:
    /// `docs` must be sorted in `[0, length)` and `docs[length]` must be
    /// [`NO_MORE_DOCS`].
    [[nodiscard]] static Result<std::shared_ptr<IntArrayDocIdSet>> create(std::vector<int32_t> docs, size_t length);

    /// Retained bytes: the id array plus the set's overhead.
    [[nodiscard]] int64_t ram_bytes_used() const override;

    [[nodiscard]] Result<std::unique_ptr<DocIdSetIterator>> iterator() const override;

    /// Always null: this set has no random-access view.
    [[nodiscard]] Result<std::shared_ptr<const Bits>> bits() const override { return std::shared_ptr<const Bits>{}; }

    [[nodiscard]] size_t length() const noexcept { return length_; }
    [[nodiscard]] const std::vector<int32_t> &docs() const noexcept { return *docs_; }

private:
    IntArrayDocIdSet(std::shared_ptr<const std::vector<int32_t>> docs, size_t length)
        : docs_(std::move(docs)), length_(length) {}

    std::shared_ptr<const std::vector<int32_t>> docs_;
    size_t length_;
};

/// Iterator over an [`IntArrayDocIdSet`].
export class IntArrayDocIdSetIterator final : public DocIdSetIterator {
public:
    IntArrayDocIdSetIterator(std::shared_ptr<const std::vector<int32_t>> docs, size_t length)
        : docs_(std::move(docs)), length_(length) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(length_); }

private:
    std::shared_ptr<const std::vector<int32_t>> docs_;
    size_t length_;
    size_t i_{0};
    int32_t doc_{-1};
};

// ---------------------------------------------------------------------------
// NotDocIdSet
// ---------------------------------------------------------------------------

/// Bits of a [`NotDocIdSet`]: the complement of the wrapped set.
export class NotDocIdBits final : public Bits {
public:
    explicit NotDocIdBits(std::shared_ptr<const Bits> in) : in_(std::move(in)) {}

    [[nodiscard]] Result<bool> get(size_t index) const override;
    [[nodiscard]] size_t length() const override { return in_->length(); }
    [[nodiscard]] std::string to_string() const override { return "NotDocIdBits(" + in_->to_string() + ")"; }

private:
    std::shared_ptr<const Bits> in_;
};

/// The negation of another [`DocIdSet`].
///
/// Port of `util/not_doc_id_set.rs`: random access is available exactly when the
/// wrapped set offers it.
export class NotDocIdSet final : public DocIdSet, public Accountable {
public:
    NotDocIdSet(int32_t max_doc, std::shared_ptr<DocIdSet> in) : max_doc_(max_doc), in_(std::move(in)) {}

    [[nodiscard]] Result<std::unique_ptr<DocIdSetIterator>> iterator() const override;
    [[nodiscard]] Result<std::shared_ptr<const Bits>> bits() const override;

    [[nodiscard]] int32_t max_doc() const noexcept { return max_doc_; }

    [[nodiscard]] int64_t ram_bytes_used() const override;

private:
    int32_t max_doc_;
    std::shared_ptr<DocIdSet> in_;
};

/// Iterator over a [`NotDocIdSet`].
export class NotDocDocIdSetIterator final : public DocIdSetIterator {
public:
    NotDocDocIdSetIterator(std::unique_ptr<DocIdSetIterator> in, int32_t max_doc)
        : in_(std::move(in)), max_doc_(max_doc) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(max_doc_); }

private:
    std::unique_ptr<DocIdSetIterator> in_;
    int32_t doc_{-1};
    int32_t next_skipped_doc_{-1};
    int32_t max_doc_;
};

// ---------------------------------------------------------------------------
// BitDocIdSet
// ---------------------------------------------------------------------------

/// Iterator over a [`BitDocIdSet`], which is a [`Bits`] over `[0, max_doc)`.
export class BitSetIterator final : public DocIdSetIterator {
public:
    BitSetIterator(std::shared_ptr<const Bits> bits, int64_t cost) : bits_(std::move(bits)), cost_(cost) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }

private:
    std::shared_ptr<const Bits> bits_;
    int64_t cost_;
    int32_t doc_{-1};
};

/// A [`DocIdSet`] backed by a [`Bits`].
///
/// Port of `util/bit_doc_id_set.rs`.  The bit set must not be modified after it
/// is handed over.
export class BitDocIdSet final : public DocIdSet, public Accountable {
public:
    /// Uses the given cost.
    [[nodiscard]] static Result<std::shared_ptr<BitDocIdSet>> create_with_cost(std::shared_ptr<const Bits> set,
                                                                              int64_t cost);
    /// Uses the cardinality of `set` when it is a [`FixedBitSet`], otherwise its
    /// length.
    [[nodiscard]] static Result<std::shared_ptr<BitDocIdSet>> create(std::shared_ptr<const Bits> set);

    [[nodiscard]] Result<std::unique_ptr<DocIdSetIterator>> iterator() const override;
    [[nodiscard]] Result<std::shared_ptr<const Bits>> bits() const override { return set_; }

    [[nodiscard]] int64_t cost() const noexcept { return cost_; }

    [[nodiscard]] int64_t ram_bytes_used() const override;

private:
    BitDocIdSet(std::shared_ptr<const Bits> set, int64_t cost) : set_(std::move(set)), cost_(cost) {}

    std::shared_ptr<const Bits> set_;
    int64_t cost_;
};

} // namespace lucene
