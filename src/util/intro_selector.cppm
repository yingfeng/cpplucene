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

export module lucene_core:intro_selector;

import :types;
import std;

import :error;
import :selector;

namespace lucene {

/// The operations an [`IntroSelector`] needs from the array it selects on.
///
/// Port of the `IntroSelectorBase`/`IntroSelectorBaseDefault` traits
/// (`util/intro_selector.rs`), flattened into one interface.
export class IntroSelectorHooks {
public:
    virtual ~IntroSelectorHooks() = default;

    /// Saves the value in slot `i` so that [`compare_pivot`] can use it.
    [[nodiscard]] virtual VoidResult set_pivot(size_t i) = 0;

    /// Compares the saved pivot with slot `j`.
    [[nodiscard]] virtual Result<int32_t> compare_pivot(size_t j) = 0;

    /// Compares slots `i` and `j`; the default round-trips through the pivot.
    [[nodiscard]] virtual Result<int32_t> compare(size_t i, size_t j);

    /// Swaps slots `i` and `j`.
    [[nodiscard]] virtual VoidResult swap(size_t i, size_t j) = 0;
};

/// Introspective quick select.
///
/// Port of `util/intro_selector.rs`: Tukey's ninther pivot (with the
/// interpolation variant when `k` is close to a boundary) and Bentley-McIlroy
/// 3-way partitioning, with a shuffle as protection against adversarial input
/// once the recursion budget is exhausted.
///
/// **Deviation:** the reference's last partition loop (`for l in last..q`) is a
/// no-op because `q <= last`; this port runs the intended `l > q` loop from the
/// original algorithm.  Both produce a valid partition, so `select` results are
/// identical.
export class IntroSelector final : public Selector {
public:
    explicit IntroSelector(IntroSelectorHooks &hooks) : hooks_(hooks) {}

    [[nodiscard]] VoidResult select(size_t from, size_t to, size_t k) override;
    [[nodiscard]] VoidResult swap(size_t i, size_t j) override { return hooks_.swap(i, j); }

    /// [`select`] with an explicit recursion budget (`2 * log2(to - from)` by
    /// default).
    [[nodiscard]] VoidResult select_in_intro(size_t from, size_t to, size_t k, int32_t max_depth);

    /// Index of the smallest of the three slots.
    [[nodiscard]] Result<size_t> min_of_three(size_t i, size_t j, size_t k);
    /// Index of the largest of the three slots.
    [[nodiscard]] Result<size_t> max_of_three(size_t i, size_t j, size_t k);
    /// Index of the median of the three slots.
    [[nodiscard]] Result<size_t> median(size_t i, size_t j, size_t k);
    /// Sorts the three slots starting at `from`.
    [[nodiscard]] VoidResult sort3(size_t from);
    /// Durstenfeld shuffle of `[from, to)`.
    [[nodiscard]] VoidResult shuffle(size_t from, size_t to);

private:
    IntroSelectorHooks &hooks_;
};

} // namespace lucene
