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

export module lucene_core:msb_radix_sorter;

import :types;
import std;

import :error;
import :intro_sorter;
import :sorter;

namespace lucene {

/// After this many levels of recursion, the radix sort falls back to introsort.
/// This protects against poor performance with long common prefixes (cache
/// locality).
export inline constexpr size_t LEVEL_THRESHOLD = 8;
/// 256 byte values plus one bucket for "the key ended".
export inline constexpr size_t HISTOGRAM_SIZE = 257;
/// Buckets of at most this many entries are sorted with the fallback sorter.
export inline constexpr size_t LENGTH_THRESHOLD = 100;

/// The key access a [`MSBRadixSorter`] needs from its delegate.
///
/// Port of the `MSBRadixSorterBase` trait (`util/most_significant_bit_radix_sort.rs`).
/// The byte-oriented algorithms are provided here (reorder, histogram,
/// `should_fallback`); a subclass only has to answer [`byte_at`].
export class MSBRadixSorterBase : public Sorter {
public:
    /// The `k`-th byte of the key in slot `i`, or `-1` when the key is shorter.
    [[nodiscard]] virtual Result<int32_t> byte_at(size_t i, size_t k);

    /// A sorter for a bucket the radix sort gives up on, comparing keys from
    /// `k` on.  The default is an introsort over [`byte_at`].
    [[nodiscard]] virtual std::unique_ptr<Sorter> get_fallback_sorter(size_t k, size_t length);

    /// Reorders the range so that each bucket occupies its `[start, end)` span;
    /// on return `start_offsets == end_offsets`.
    [[nodiscard]] virtual VoidResult reorder(size_t from, size_t to, std::span<size_t> start_offsets,
                                             std::span<size_t> end_offsets, size_t k);

    /// The bucket slot of the key at `i`: `byte_at(i, k) + 1`.
    [[nodiscard]] virtual Result<size_t> get_bucket(size_t i, size_t k);

    /// Counts the buckets of `[from, to)`, seeding the common-prefix bucket.
    [[nodiscard]] virtual VoidResult build_histogram(size_t prefix_common_bucket, size_t prefix_common_len,
                                                     size_t from, size_t to, size_t k,
                                                     std::span<size_t> histogram);

    /// `true` when the range is small enough, or the recursion deep enough, that
    /// introsort is cheaper than another radix level.
    [[nodiscard]] virtual bool should_fallback(size_t from, size_t to, size_t level);
};

/// Most-significant-byte radix sort over byte keys.
///
/// Port of `MSBRadixSorter` (`util/most_significant_bit_radix_sort.rs`).  Each
/// level computes the longest common prefix of the range and then buckets the
/// entries on the next byte, recursing per bucket.  Small ranges (and ranges
/// with long common prefixes) fall back to introsort.
///
/// Not stable.
export class MSBRadixSorter final : public Sorter {
public:
    /// `max_length` is the length of the longest key, or
    /// `INT32_MAX` when unknown.
    MSBRadixSorter(size_t max_length, std::unique_ptr<MSBRadixSorterBase> delegate);

    [[nodiscard]] VoidResult sort(size_t from, size_t to) override;

    [[nodiscard]] VoidResult sort_impl(size_t from, size_t to, size_t k, size_t level);

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override { return delegate_->swap(i, j); }
    [[nodiscard]] VoidResult set_pivot(size_t i) override { return delegate_->set_pivot(i); }
    [[nodiscard]] Result<int32_t> compare_pivot(size_t j) override { return delegate_->compare_pivot(j); }

    [[nodiscard]] const MSBRadixSorterBase &delegate() const noexcept { return *delegate_; }

private:
    [[nodiscard]] bool should_fallback(size_t from, size_t to, size_t level);
    [[nodiscard]] Result<size_t> compute_initial_common_prefix_length(size_t from, size_t k);
    [[nodiscard]] Result<size_t> compute_common_prefix_length_and_build_histogram_part1(size_t from, size_t to,
                                                                                       size_t k, size_t level,
                                                                                       size_t common_prefix_length);
    [[nodiscard]] Result<size_t> compute_common_prefix_length_and_build_histogram_part2(size_t from, size_t to,
                                                                                       size_t k, size_t level,
                                                                                       size_t common_prefix_length,
                                                                                       size_t i);
    [[nodiscard]] Result<size_t> compute_common_prefix_length_and_build_histogram(size_t from, size_t to, size_t k,
                                                                                 size_t level);
    static void sum_histogram(std::span<size_t> histogram, std::span<size_t> end_offsets);
    [[nodiscard]] VoidResult reorder(size_t from, size_t to, size_t level, size_t k);
    [[nodiscard]] VoidResult radix_sort(size_t from, size_t to, size_t k, size_t level);

    std::unique_ptr<MSBRadixSorterBase> delegate_;
    /// One histogram per recursion level.
    std::array<std::vector<size_t>, LEVEL_THRESHOLD> histograms_;
    /// End offsets for the current histogram.
    std::array<size_t, HISTOGRAM_SIZE> end_offsets_{};
    /// Longest common prefix of the current range, at most 24 bytes.
    std::array<int32_t, 24> common_prefix_{};
    size_t max_length_{0};
};

} // namespace lucene
