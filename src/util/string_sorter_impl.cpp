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

module lucene_core:string_sorter.impl;

import :string_sorter;

import :bytes_ref;
import :bytes_ref_comparator;
import :error;
import :intro_sorter;
import :msb_radix_sorter;
import :sorter;
import :types;

import std;

namespace lucene {

namespace {

/// Introsort over a [`StringSorterBase`]'s keys.
///
/// Port of `IntroSorterImpl` (`util/string_sorter.rs`).
class StringSorterIntroSorter final : public IntroSorter {
public:
    StringSorterIntroSorter(StringSorterBase &delegate, BytesRefComparator &cmp, std::optional<size_t> k)
            : delegate_(delegate), cmp_(cmp), k_(k) {}

    [[nodiscard]] Result<int32_t> compare(size_t i, size_t j) override {
        auto left = delegate_.get(i);
        if (!left.has_value()) {
            return std::unexpected(std::move(left.error()));
        }
        auto right = delegate_.get(j);
        if (!right.has_value()) {
            return std::unexpected(std::move(right.error()));
        }
        return compare_keys(left.value(), right.value());
    }

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override { return delegate_.swap(i, j); }

    [[nodiscard]] VoidResult set_pivot(size_t i) override {
        auto key = delegate_.get(i);
        if (!key.has_value()) {
            return std::unexpected(std::move(key.error()));
        }
        pivot_ = std::move(key.value());
        return {};
    }

    [[nodiscard]] Result<int32_t> compare_pivot(size_t j) override {
        auto key = delegate_.get(j);
        if (!key.has_value()) {
            return std::unexpected(std::move(key.error()));
        }
        return compare_keys(pivot_, key.value());
    }

private:
    [[nodiscard]] int32_t compare_keys(const BytesRef &a, const BytesRef &b) const {
        return k_.has_value() ? cmp_.compare_with_offset(a, b, k_.value()) : cmp_.compare(a, b);
    }

    StringSorterBase &delegate_;
    BytesRefComparator &cmp_;
    std::optional<size_t> k_;
    BytesRef pivot_{};
};

/// Adapter that exposes a [`StringSorterBase`] to the radix sorter.
///
/// Port of `MSBStringRadixSorter` (`util/string_sorter.rs`).
class MSBStringRadixSorter final : public MSBRadixSorterBase {
public:
    MSBStringRadixSorter(StringSorterBase &delegate, BytesRefComparator &cmp) : delegate_(delegate), cmp_(cmp) {}

    [[nodiscard]] Result<int32_t> byte_at(size_t i, size_t k) override {
        auto key = delegate_.get(i);
        if (!key.has_value()) {
            return std::unexpected(std::move(key.error()));
        }
        return cmp_.byte_at(key.value(), k);
    }

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override { return delegate_.swap(i, j); }

    [[nodiscard]] std::unique_ptr<Sorter> get_fallback_sorter(size_t k, size_t /*length*/) override {
        return delegate_.fall_back_sorter(cmp_, std::optional<size_t>{k});
    }

private:
    StringSorterBase &delegate_;
    BytesRefComparator &cmp_;
};

} // namespace

std::unique_ptr<Sorter> StringSorterBase::fall_back_sorter(BytesRefComparator &cmp, std::optional<size_t> k) {
    return std::make_unique<StringSorterIntroSorter>(*this, cmp, k);
}

std::unique_ptr<Sorter> StringSorterBase::radix_sorter(BytesRefComparator &cmp) {
    const size_t length = cmp.compared_bytes_count();
    return std::make_unique<MSBRadixSorter>(length, std::make_unique<MSBStringRadixSorter>(*this, cmp));
}

Result<int32_t> StringSorter::compare(size_t i, size_t j) {
    auto left = delegate_->get(i);
    if (!left.has_value()) {
        return std::unexpected(std::move(left.error()));
    }
    auto right = delegate_->get(j);
    if (!right.has_value()) {
        return std::unexpected(std::move(right.error()));
    }
    return cmp_.compare(left.value(), right.value());
}

VoidResult StringSorter::sort(size_t from, size_t to) {
    // The reference implementation picks the radix sorter only when the comparator is the plain byte
    // comparator (`C::TYPE == BYTES_REF_COMPARATOR_TYPE`); every comparator in
    // this port is a `BytesRefComparator`, so that check is structural.
    auto sorter = delegate_->radix_sorter(cmp_);
    return sorter->sort(from, to);
}

} // namespace lucene
