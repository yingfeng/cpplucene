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

module lucene_core:intro_sorter.impl;

import :intro_sorter;

import :error;
import :sorter;
import :types;

import std;

namespace lucene {

VoidResult IntroSorter::sort_range(size_t from, size_t to) {
    if (auto checked = check_range(from, to); !checked.has_value()) {
        return checked;
    }
    // 2 * floor(log2(to - from)), or 0 for ranges shorter than two elements.
    size_t max_depth = 0;
    if (to > from + 1) {
        size_t length = to - from;
        size_t log2 = 0;
        while (length > 1) {
            length >>= 1;
            ++log2;
        }
        max_depth = 2 * log2;
    }
    return sort_in_intro(from, to, max_depth);
}

VoidResult IntroSorter::sort_in_intro(size_t from, size_t to, size_t max_depth) {
    while (to - from > INSERTION_SORT_THRESHOLD) {
        if (max_depth == 0) {
            // Max recursion depth exceeded: fall back to heap sort.
            return heap_sort(from, to);
        }
        max_depth -= 1;

        const size_t size = to - from;
        const size_t last = to - 1;
        const size_t mid = (from + last) >> 1;

        size_t pivot = 0;
        if (size <= SINGLE_MEDIAN_THRESHOLD) {
            // A single median around the middle element.  The medians of
            // [from, mid, last] are deliberately not used: that hurts when the
            // input is descending and combined with 3-way partitioning.
            const size_t range = size >> 2;
            auto selected = median(mid - range, mid, mid + range);
            if (!selected.has_value()) {
                return std::unexpected(std::move(selected.error()));
            }
            pivot = selected.value();
        } else {
            // Tukey's ninther.
            const size_t range = size >> 3;
            const size_t double_range = range << 1;
            auto first = median(from, from + range, from + double_range);
            if (!first.has_value()) {
                return std::unexpected(std::move(first.error()));
            }
            auto middle = median(mid - range, mid, mid + range);
            if (!middle.has_value()) {
                return std::unexpected(std::move(middle.error()));
            }
            auto final = median(last - double_range, last - range, last);
            if (!final.has_value()) {
                return std::unexpected(std::move(final.error()));
            }
            auto selected = median(first.value(), middle.value(), final.value());
            if (!selected.has_value()) {
                return std::unexpected(std::move(selected.error()));
            }
            pivot = selected.value();
        }

        if (auto pivoted = set_pivot(pivot); !pivoted.has_value()) {
            return pivoted;
        }
        if (auto swapped = swap(from, pivot); !swapped.has_value()) {
            return swapped;
        }

        size_t i = from;
        size_t j = to;
        size_t p = from + 1;
        size_t q = last;

        int32_t left_cmp = 0;
        int32_t right_cmp = 0;
        while (true) {
            do {
                i += 1;
                auto cmp = compare_pivot(i);
                if (!cmp.has_value()) {
                    return std::unexpected(std::move(cmp.error()));
                }
                left_cmp = cmp.value();
            } while (left_cmp > 0);

            do {
                j -= 1;
                auto cmp = compare_pivot(j);
                if (!cmp.has_value()) {
                    return std::unexpected(std::move(cmp.error()));
                }
                right_cmp = cmp.value();
            } while (right_cmp < 0);

            if (i >= j) {
                if (i == j && right_cmp == 0) {
                    if (auto swapped = swap(i, p); !swapped.has_value()) {
                        return swapped;
                    }
                }
                break;
            }

            if (auto swapped = swap(i, j); !swapped.has_value()) {
                return swapped;
            }
            if (right_cmp == 0) {
                if (auto swapped = swap(i, p); !swapped.has_value()) {
                    return swapped;
                }
                p += 1;
            }
            if (left_cmp == 0) {
                if (auto swapped = swap(j, q); !swapped.has_value()) {
                    return swapped;
                }
                q -= 1;
            }
        }

        i = j + 1;

        size_t left_end = j + 1;
        for (size_t k = from; k < p; ++k) {
            if (auto swapped = swap(k, left_end - 1); !swapped.has_value()) {
                return swapped;
            }
            left_end -= 1;
        }

        for (size_t k = last; k > q; --k) {
            if (auto swapped = swap(k, i); !swapped.has_value()) {
                return swapped;
            }
            i += 1;
        }

        // Recurse on the smaller partition and continue with the larger one.
        if (left_end - from < to - i) {
            if (auto sorted = sort_in_intro(from, left_end, max_depth); !sorted.has_value()) {
                return sorted;
            }
            from = i;
        } else {
            if (auto sorted = sort_in_intro(i, to, max_depth); !sorted.has_value()) {
                return sorted;
            }
            to = left_end;
        }
    }

    return insertion_sort(from, to);
}

Result<size_t> IntroSorter::median(size_t i, size_t j, size_t k) {
    auto ij = compare(i, j);
    if (!ij.has_value()) {
        return std::unexpected(std::move(ij.error()));
    }
    if (ij.value() < 0) {
        auto jk = compare(j, k);
        if (!jk.has_value()) {
            return std::unexpected(std::move(jk.error()));
        }
        if (jk.value() <= 0) {
            return j;
        }
        auto ik = compare(i, k);
        if (!ik.has_value()) {
            return std::unexpected(std::move(ik.error()));
        }
        return ik.value() < 0 ? k : i;
    }
    auto jk = compare(j, k);
    if (!jk.has_value()) {
        return std::unexpected(std::move(jk.error()));
    }
    if (jk.value() >= 0) {
        return j;
    }
    auto ik = compare(i, k);
    if (!ik.has_value()) {
        return std::unexpected(std::move(ik.error()));
    }
    return ik.value() < 0 ? i : k;
}

} // namespace lucene
