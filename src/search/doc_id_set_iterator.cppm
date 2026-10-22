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

export module lucene_core:doc_id_set_iterator;

import :types;
import std;

import :error;
import :fixed_bit_set;

namespace lucene {

/// The sentinel returned by [`DocIdSetIterator`] once the iterator is
/// exhausted (`i32::MAX`).
export constexpr int32_t NO_MORE_DOCS = std::numeric_limits<int32_t>::max();

/// Random access supported by bit-set iterators.  Port of the
/// `BitSetIteratorAccess` trait (`core/search/doc_id_set_iterator.rs`).
///
/// Ordinary iterators keep the defaults; ownership wrappers forward these
/// methods only when they preserve the underlying iterator capability.
export class BitSetIteratorAccess {
public:
    virtual ~BitSetIteratorAccess() = default;

    [[nodiscard]] virtual bool is_bit_iter() const { return false; }

    [[nodiscard]] virtual Result<bool> get(size_t /*index*/) const {
        return err(LuceneError::unsupported_operation("Iterator does not support bit-set access"));
    }

    [[nodiscard]] virtual VoidResult set_doc_id(int32_t /*doc*/) {
        return err(LuceneError::unsupported_operation("Iterator does not support setting its document ID"));
    }

    [[nodiscard]] virtual Result<size_t> bit_set_length() const {
        return err(LuceneError::unsupported_operation("Iterator does not expose a bit-set length"));
    }
};

/// Specialisation hooks for [`DocIdSetIterator`].  Port of the
/// `DocIdSetIteratorExtensions` trait.
///
/// Deviation from the reference implementation: the `SparseFixedBitSet` hook is not ported yet
/// (there is no `:sparse_fixed_bit_set` partition), so only the fixed bit set
/// and the doc-base fixed bit set accessors exist.
export class DocIdSetIteratorExtensions {
public:
    virtual ~DocIdSetIteratorExtensions() = default;

    /// The wrapped fixed bit set when this iterator exposes that
    /// specialisation.
    [[nodiscard]] virtual const FixedBitSet *get_fixed_bit_set() const { return nullptr; }

    /// The document base and backing bit set of a `DocBaseBitSetIterator`.
    [[nodiscard]] virtual std::optional<std::pair<size_t, const FixedBitSet *>>
    get_doc_base_fixed_bit_set() const {
        return std::nullopt;
    }
};

/// Iterates over a set of non-decreasing document ids.  Port of the
/// `DocIdSetIterator` trait.
///
/// Implementations treat `i32::MAX` ([`NO_MORE_DOCS`]) as an invalid value.
export class DocIdSetIterator : public DocIdSetIteratorExtensions, public BitSetIteratorAccess {
public:
    /// Returns `-1` when neither [`next_doc`] nor [`advance`] has been called
    /// yet, [`NO_MORE_DOCS`] when the iterator is exhausted, and the current
    /// document id otherwise.
    [[nodiscard]] virtual int32_t doc_id() const = 0;

    /// Advances to the next document and returns it, or [`NO_MORE_DOCS`].
    [[nodiscard]] virtual Result<int32_t> next_doc() = 0;

    /// Advances to the first document `>= target` and returns it, or
    /// [`NO_MORE_DOCS`].  The default implementation mirrors the reference implementation and
    /// reports `not_implemented`.
    [[nodiscard]] virtual Result<int32_t> advance(int32_t target);

    /// A slow (linear) implementation of [`advance`] that relies on
    /// [`next_doc`] to move beyond the target position.
    [[nodiscard]] virtual Result<int32_t> slow_advance(int32_t target);

    /// Estimated cost of this iterator; the default implementation mirrors
    /// The reference implementation and reports `not_implemented`.
    [[nodiscard]] virtual Result<int64_t> cost() const;
};

/// An empty [`DocIdSetIterator`].
export class EmptyDISI : public DocIdSetIterator {
public:
    [[nodiscard]] int32_t doc_id() const override { return exhausted_ ? NO_MORE_DOCS : -1; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return int64_t{0}; }

private:
    bool exhausted_{false};
};

/// A [`DocIdSetIterator`] that matches all documents up to `max_doc - 1`.
export class AllDISI : public DocIdSetIterator {
public:
    explicit AllDISI(int32_t max_doc) : max_doc_(max_doc) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(max_doc_); }

private:
    int32_t doc_{-1};
    int32_t max_doc_;
};

/// A [`DocIdSetIterator`] that matches a range of documents from `min_doc_id`
/// (inclusive) to `max_doc_id` (exclusive).
export class RangeDISI : public DocIdSetIterator {
public:
    /// Returns `illegal_argument` when `min_doc_id >= max_doc_id` or
    /// `min_doc_id < 0`.
    [[nodiscard]] static Result<RangeDISI> create(int32_t min_doc_id, int32_t max_doc_id);

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override {
        return static_cast<int64_t>(max_doc_) - static_cast<int64_t>(min_doc_);
    }

private:
    RangeDISI(int32_t min_doc, int32_t max_doc) : min_doc_(min_doc), max_doc_(max_doc) {}

    int32_t doc_{-1};
    int32_t min_doc_;
    int32_t max_doc_;
};

/// The `either_docidsetiterator_named!` enum variants of the reference implementation
/// (`DocIdSetIteratorEnum2` .. `DocIdSetIteratorEnum5`), expressed as one
/// variadic type.  Ownership is by value, like the Rust enum.
export template <class... Ts>
class DocIdSetIteratorEnum : public DocIdSetIterator {
public:
    explicit DocIdSetIteratorEnum(std::variant<Ts...> inner) : inner_(std::move(inner)) {}

    [[nodiscard]] int32_t doc_id() const override {
        return std::visit([](const auto &iterator) { return iterator.doc_id(); }, inner_);
    }

    [[nodiscard]] Result<int32_t> next_doc() override {
        return std::visit([](auto &iterator) { return iterator.next_doc(); }, inner_);
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        return std::visit([target](auto &iterator) { return iterator.advance(target); }, inner_);
    }

    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override {
        return std::visit([target](auto &iterator) { return iterator.slow_advance(target); }, inner_);
    }

    [[nodiscard]] Result<int64_t> cost() const override {
        return std::visit([](const auto &iterator) { return iterator.cost(); }, inner_);
    }

    [[nodiscard]] bool is_bit_iter() const override {
        return std::visit([](const auto &iterator) { return iterator.is_bit_iter(); }, inner_);
    }

    [[nodiscard]] Result<bool> get(size_t index) const override {
        return std::visit([index](const auto &iterator) { return iterator.get(index); }, inner_);
    }

    [[nodiscard]] VoidResult set_doc_id(int32_t doc) override {
        return std::visit([doc](auto &iterator) { return iterator.set_doc_id(doc); }, inner_);
    }

    [[nodiscard]] Result<size_t> bit_set_length() const override {
        return std::visit([](const auto &iterator) { return iterator.bit_set_length(); }, inner_);
    }

private:
    std::variant<Ts...> inner_;
};

} // namespace lucene
