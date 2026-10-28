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

export module lucene_core:lsb_radix_sorter;

import :types;
import std;

import :error;

namespace lucene {

/// Below this many values the sorter uses an insertion sort.
export inline constexpr size_t LSB_INSERTION_SORT_THRESHOLD = 30;

/// LSD radix sort for `int32_t` values, byte by byte.
///
/// Port of `util/lsb_radix_sorter.rs`.  The values are compared as unsigned, so
/// this only produces the natural order for non-negative values (which is what
/// the postings writer needs).  Reusable: the bucket buffer is kept between
/// calls.
export class LSBRadixSorter {
public:
    LSBRadixSorter() = default;

    /// Sorts `array[0, len)` in place.
    ///
    /// `num_bits` is the number of bits needed by any value (pass 32 when
    /// unknown).
    [[nodiscard]] VoidResult sort(size_t num_bits, std::span<int32_t> array, size_t len);

private:
    static void build_histogram(std::span<const int32_t> array, size_t len, std::span<size_t, 256> histogram,
                                size_t shift);
    static void sum_histogram(std::span<size_t, 256> histogram);
    static void reorder(std::span<const int32_t> array, size_t len, std::span<size_t, 256> histogram, size_t shift,
                        std::span<int32_t> dest);
    /// Returns false when every value has the same byte at `shift`.
    static bool sort_pass(std::span<const int32_t> array, size_t len, std::span<size_t, 256> histogram, size_t shift,
                          std::span<int32_t> dest);
    static void insertion_sort(std::span<int32_t> array, size_t off, size_t len);

    std::array<size_t, 256> histogram_{};
    std::vector<int32_t> buffer_;
};

} // namespace lucene
