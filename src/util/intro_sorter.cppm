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

export module lucene_core:intro_sorter;

import :types;
import std;

import :sorter;

namespace lucene {

/// Below this size, the pivot selection is simplified to a single median.
export inline constexpr size_t SINGLE_MEDIAN_THRESHOLD = 40;

/// Introsort: quicksort that falls back to heapsort once the recursion is
/// deeper than `2 * log2(n)`.
///
/// Port of `util/intro_sorter.rs`.  The pivot is chosen with Tukey's ninther
/// (median of medians) and the partition is the Bentley-McIlroy 3-way one, so
/// ranges with many equal keys stay linear.  Small ranges use insertion sort.
///
/// Not stable.
export class IntroSorter : public Sorter {
public:
    /// Sorts `[from, to)`; this is what [`sort`] does.
    [[nodiscard]] VoidResult sort(size_t from, size_t to) override { return sort_range(from, to); }

    [[nodiscard]] VoidResult sort_range(size_t from, size_t to);

    /// Sorts `[from, to)` allowing `max_depth` further recursion levels.
    [[nodiscard]] VoidResult sort_in_intro(size_t from, size_t to, size_t max_depth);

    /// Index of the median of the three slots `i`, `j`, `k`.
    [[nodiscard]] Result<size_t> median(size_t i, size_t j, size_t k);
};

} // namespace lucene
