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

export module lucene_core:radix_selector;

import :types;
import std;

import :error;
import :intro_selector;
import :selector;

namespace lucene {

/// Number of histogram buckets: 256 byte values plus "value ended".
export inline constexpr size_t RADIX_HISTOGRAM_SIZE = 257;

/// The operations a [`RadixSelector`] needs from the array it selects on.
///
/// Port of the `RadixSelectorBase` trait (`util/radix_selector.rs`).  The
/// fall-back selector of the reference is built inside [`RadixSelector`] from
/// these two operations, so it is not part of the interface.
export class RadixSelectorHooks {
public:
    virtual ~RadixSelectorHooks() = default;

    /// The `k`-th byte of slot `i`, or `-1` when the value is shorter.
    ///
    /// `k` is always in `[0, max_length)`.
    [[nodiscard]] virtual Result<int32_t> byte_at(size_t i, size_t k) = 0;

    /// Swaps slots `i` and `j`.
    [[nodiscard]] virtual VoidResult swap(size_t i, size_t j) = 0;
};

/// MSB radix select over byte keys.
///
/// Port of `util/radix_selector.rs`: buckets on the `d`-th byte, following the
/// bucket that contains `k`, and falls back to introselect for narrow ranges or
/// deep recursion (long common prefixes).
export class RadixSelector final : public Selector {
public:
    /// Buckets at most this large are selected with introselect.
    static constexpr size_t LENGTH_THRESHOLD = 100;
    /// After this many levels the radix pass falls back to introselect.
    static constexpr size_t LEVEL_THRESHOLD = 8;

    RadixSelector(size_t max_length, RadixSelectorHooks &hooks);

    [[nodiscard]] VoidResult select(size_t from, size_t to, size_t k) override;
    [[nodiscard]] VoidResult swap(size_t i, size_t j) override { return hooks_.swap(i, j); }

    /// [`select`] starting at byte offset `d` with recursion level `l`.
    [[nodiscard]] VoidResult radix_select(size_t from, size_t to, size_t k, size_t d, size_t level);

    [[nodiscard]] size_t max_length() const noexcept { return max_length_; }

private:
    [[nodiscard]] VoidResult select_impl(size_t from, size_t to, size_t k, size_t d, size_t level);
    [[nodiscard]] Result<size_t> get_bucket(size_t i, size_t k);
    [[nodiscard]] Result<size_t> compute_common_prefix_length_and_build_histogram(size_t from,
                                                                                 size_t to,
                                                                                 size_t k);
    [[nodiscard]] Result<size_t> compute_initial_common_prefix_length(size_t from, size_t k);
    [[nodiscard]] VoidResult build_histogram(size_t from, size_t to, size_t k);
    [[nodiscard]] VoidResult partition(size_t from,
                                       size_t to,
                                       size_t bucket,
                                       size_t bucket_from,
                                       size_t bucket_to,
                                       size_t d);

    size_t max_length_;
    std::vector<int32_t> common_prefix_;
    std::array<size_t, RADIX_HISTOGRAM_SIZE> histogram_{};
    RadixSelectorHooks &hooks_;
};

} // namespace lucene
