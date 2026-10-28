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

export module lucene_core:stable_msb_radix_sorter;

import :types;
import std;

import :error;
import :msb_radix_sorter;
import :sorter;

namespace lucene {

/// [`MSBRadixSorterBase`] plus the temporary storage a *stable* radix pass
/// needs.
///
/// Port of the `StableMSBRadixSorterBase` trait
/// (`util/stable_msb_radix_sorter.rs`).  Unlike [`MSBRadixSorterBase`], which
/// swaps values in place, reordering here copies values into a scratch area and
/// then back, which keeps equal keys in their original order.
export class StableMSBRadixSorterHooks : public MSBRadixSorterBase {
public:
    /// Copies the value in slot `i` into scratch slot `j`.
    [[nodiscard]] virtual VoidResult save(size_t i, size_t j) = 0;

    /// Copies scratch slots `[i, j)` back into the array, in place.
    [[nodiscard]] virtual VoidResult restore(size_t i, size_t j) = 0;

    /// A stable sorter for a bucket the radix sort gives up on, comparing keys
    /// from byte `k` on.
    ///
    /// The default is a merge sort over [`save`]/[`restore`].
    [[nodiscard]] virtual std::unique_ptr<Sorter> stable_fallback_sorter(size_t k, size_t max_length);
};

/// Stable MSB radix sort over byte keys.
///
/// Port of `StableMSBRadixSorter` (`util/stable_msb_radix_sorter.rs`): the same
/// bucketing as [`MSBRadixSorter`] with a stable [`reorder`] and a merge-sort
/// fallback, so equal keys keep their input order.
export class StableMSBRadixSorter final : public MSBRadixSorterBase {
public:
    StableMSBRadixSorter(std::unique_ptr<StableMSBRadixSorterHooks> delegate, size_t max_length);

    [[nodiscard]] Result<int32_t> byte_at(size_t i, size_t k) override { return delegate_->byte_at(i, k); }
    [[nodiscard]] std::unique_ptr<Sorter> get_fallback_sorter(size_t k, size_t length) override;
    [[nodiscard]] Result<size_t> get_bucket(size_t i, size_t k) override { return delegate_->get_bucket(i, k); }
    [[nodiscard]] VoidResult build_histogram(size_t prefix_common_bucket, size_t prefix_common_len, size_t from,
                                             size_t to, size_t k, std::span<size_t> histogram) override;
    [[nodiscard]] bool should_fallback(size_t from, size_t to, size_t level) override {
        return delegate_->should_fallback(from, to, level);
    }

    [[nodiscard]] VoidResult reorder(size_t from, size_t to, std::span<size_t> start_offsets,
                                     std::span<size_t> end_offsets, size_t k) override;

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override { return delegate_->swap(i, j); }
    [[nodiscard]] VoidResult set_pivot(size_t i) override { return delegate_->set_pivot(i); }
    [[nodiscard]] Result<int32_t> compare_pivot(size_t j) override { return delegate_->compare_pivot(j); }

    [[nodiscard]] size_t max_length() const noexcept { return max_length_; }

private:
    std::unique_ptr<StableMSBRadixSorterHooks> delegate_;
    std::array<size_t, HISTOGRAM_SIZE> fixed_start_offsets_{};
    size_t max_length_;
};

} // namespace lucene
