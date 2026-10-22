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

export module lucene_core:string_sorter;

import :types;
import std;

import :bytes_ref;
import :bytes_ref_comparator;
import :error;
import :sorter;

namespace lucene {

/// Sorts slots whose values are byte keys, using a [`BytesRefComparator`].
///
/// Port of the `StringSorterBase` trait (`util/string_sorter.rs`).  A subclass
/// only has to provide the key of a slot ([`get`]); swapping and the two
/// sorting strategies come for free.
///
/// **Deviation:** the reference implementation writes the key into a borrowed scratch buffer so a
/// sort allocates nothing; here [`get`] returns an owned `BytesRef`, which costs
/// one copy per comparison.  The algorithms and their ordering are identical.
export class StringSorterBase : public Sorter {
public:
    /// The key stored in slot `i`.
    [[nodiscard]] virtual Result<BytesRef> get(size_t i) = 0;

    /// Introsort over the keys, comparing from byte `k` on.
    [[nodiscard]] virtual std::unique_ptr<Sorter> fall_back_sorter(BytesRefComparator &cmp, std::optional<size_t> k);

    /// MSB radix sort over the keys.
    [[nodiscard]] virtual std::unique_ptr<Sorter> radix_sorter(BytesRefComparator &cmp);
};

/// The [`Sorter`] that drives a [`StringSorterBase`]: it prefers the radix
/// sorter, which is only applicable when the comparator is a plain byte
/// comparator (the natural order).
///
/// Port of `StringSorter` (`util/string_sorter.rs`).
export class StringSorter final : public Sorter {
public:
    StringSorter(std::unique_ptr<StringSorterBase> delegate, BytesRefComparator &cmp)
            : delegate_(std::move(delegate)), cmp_(cmp) {}

    [[nodiscard]] Result<int32_t> compare(size_t i, size_t j) override;
    [[nodiscard]] VoidResult swap(size_t i, size_t j) override { return delegate_->swap(i, j); }
    [[nodiscard]] VoidResult sort(size_t from, size_t to) override;

private:
    std::unique_ptr<StringSorterBase> delegate_;
    BytesRefComparator &cmp_;
};

} // namespace lucene
