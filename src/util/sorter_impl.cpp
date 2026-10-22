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

module lucene_core:sorter.impl;

import :sorter;

import :debug;
import :error;
import :types;

import std;

namespace lucene {

VoidResult check_range(size_t from, size_t to) {
    if (to < from) {
        return err(LuceneError::illegal_argument("'to' must be >= 'from', got from= " + std::to_string(from) +
                                                 " and to= " + std::to_string(to)));
    }
    return {};
}

Result<int32_t> Sorter::compare(size_t /*i*/, size_t /*j*/) {
    return err(LuceneError::illegal_state("compare() must be implemented if it needs to be used"));
}

VoidResult Sorter::swap(size_t /*i*/, size_t /*j*/) {
    return err(LuceneError::illegal_state("swap() must be implemented if it needs to be used"));
}

VoidResult Sorter::set_pivot(size_t /*i*/) {
    return err(LuceneError::illegal_state("set_pivot() must be implemented if it needs to be used"));
}

Result<int32_t> Sorter::compare_pivot(size_t /*j*/) {
    return err(LuceneError::illegal_state("compare_pivot() must be implemented if it needs to be used"));
}

VoidResult Sorter::sort(size_t /*from*/, size_t /*to*/) {
    return err(LuceneError::illegal_state("sort() must be implemented if it needs to be used"));
}

VoidResult Sorter::merge_in_place(size_t from, size_t mid, size_t to) {
    if (from == mid || mid == to) {
        return {};
    }
    auto boundary = compare(mid - 1, mid);
    if (!boundary.has_value()) {
        return std::unexpected(std::move(boundary.error()));
    }
    if (boundary.value() <= 0) {
        return {};
    }
    if (to == 2 + from) {
        return swap(mid - 1, mid);
    }

    while (true) {
        auto cmp = compare(from, mid);
        if (!cmp.has_value()) {
            return std::unexpected(std::move(cmp.error()));
        }
        if (cmp.value() > 0) {
            break;
        }
        from += 1;
    }
    while (true) {
        auto cmp = compare(mid - 1, to - 1);
        if (!cmp.has_value()) {
            return std::unexpected(std::move(cmp.error()));
        }
        if (cmp.value() > 0) {
            break;
        }
        to -= 1;
    }

    size_t first_cut = 0;
    size_t second_cut = 0;
    size_t len22 = 0;
    if (mid - from > to - mid) {
        const size_t len11 = (mid - from) >> 1;
        first_cut = from + len11;
        auto cut = lower(mid, to, first_cut);
        if (!cut.has_value()) {
            return std::unexpected(std::move(cut.error()));
        }
        second_cut = cut.value();
        len22 = second_cut - mid;
    } else {
        len22 = (to - mid) >> 1;
        second_cut = mid + len22;
        auto cut = upper(from, mid, second_cut);
        if (!cut.has_value()) {
            return std::unexpected(std::move(cut.error()));
        }
        first_cut = cut.value();
    }

    if (auto rotated = rotate(first_cut, mid, second_cut); !rotated.has_value()) {
        return rotated;
    }
    const size_t new_mid = first_cut + len22;
    if (auto merged = merge_in_place(from, first_cut, new_mid); !merged.has_value()) {
        return merged;
    }
    return merge_in_place(new_mid, second_cut, to);
}

Result<size_t> Sorter::lower(size_t from, size_t to, size_t val) {
    size_t len = to - from;
    while (len > 0) {
        const size_t half = len >> 1;
        const size_t mid = from + half;
        auto cmp = compare(mid, val);
        if (!cmp.has_value()) {
            return std::unexpected(std::move(cmp.error()));
        }
        if (cmp.value() < 0) {
            from = mid + 1;
            len = len - half - 1;
        } else {
            len = half;
        }
    }
    return from;
}

Result<size_t> Sorter::upper(size_t from, size_t to, size_t val) {
    size_t len = to - from;
    while (len > 0) {
        const size_t half = len >> 1;
        const size_t mid = from + half;
        auto cmp = compare(val, mid);
        if (!cmp.has_value()) {
            return std::unexpected(std::move(cmp.error()));
        }
        if (cmp.value() < 0) {
            len = half;
        } else {
            from = mid + 1;
            len = len - half - 1;
        }
    }
    return from;
}

Result<size_t> Sorter::lower2(size_t from, size_t to, size_t val) {
    size_t f = to - 1;
    size_t t = to;

    while (f > from) {
        auto cmp = compare(f, val);
        if (!cmp.has_value()) {
            return std::unexpected(std::move(cmp.error()));
        }
        if (cmp.value() < 0) {
            return lower(f, t, val);
        }
        const size_t delta = t - f;
        t = f;
        const size_t doubled = delta << 1;
        f = f > doubled ? f - doubled : 0;
    }

    return lower(from, t, val);
}

Result<size_t> Sorter::upper2(size_t from, size_t to, size_t val) {
    size_t f = from;
    size_t t = f + 1;

    while (t < to) {
        auto cmp = compare(t, val);
        if (!cmp.has_value()) {
            return std::unexpected(std::move(cmp.error()));
        }
        if (cmp.value() > 0) {
            return upper(f, t, val);
        }
        const size_t delta = t - f;
        f = t;
        t += delta << 1;
    }

    return upper(f, to, val);
}

VoidResult Sorter::reverse(size_t from, size_t to) {
    if (to == 0) {
        return {};
    }
    size_t last = to - 1;
    while (true) {
        if (from >= last) {
            break;
        }
        if (auto swapped = swap(from, last); !swapped.has_value()) {
            return swapped;
        }
        from += 1;
        if (last == 0) {
            break;
        }
        last -= 1;
    }
    return {};
}

VoidResult Sorter::rotate(size_t lo, size_t mid, size_t hi) {
    debug_check(lo <= mid && mid <= hi, "rotate requires lo <= mid <= hi");
    if (lo == mid || mid == hi) {
        return {};
    }
    return do_rotate(lo, mid, hi);
}

VoidResult Sorter::do_rotate(size_t lo, size_t mid, size_t hi) {
    if (mid + mid == hi + lo) {
        while (mid < hi) {
            if (auto swapped = swap(lo, mid); !swapped.has_value()) {
                return swapped;
            }
            lo += 1;
            mid += 1;
        }
    } else {
        if (auto reversed = reverse(lo, mid); !reversed.has_value()) {
            return reversed;
        }
        if (auto reversed = reverse(mid, hi); !reversed.has_value()) {
            return reversed;
        }
        if (auto reversed = reverse(lo, hi); !reversed.has_value()) {
            return reversed;
        }
    }
    return {};
}

VoidResult Sorter::binary_sort(size_t from, size_t to) { return binary_sort_with_start(from, to, from + 1); }

VoidResult Sorter::binary_sort_with_start(size_t from, size_t to, size_t start) {
    for (size_t i = start; i < to; ++i) {
        if (auto pivoted = set_pivot(i); !pivoted.has_value()) {
            return pivoted;
        }
        size_t l = from;
        size_t h = i;
        while (l < h) {
            const size_t mid = l + ((h - l - 1) >> 1);
            auto cmp = compare_pivot(mid);
            if (!cmp.has_value()) {
                return std::unexpected(std::move(cmp.error()));
            }
            if (cmp.value() < 0) {
                h = mid;
            } else {
                l = mid + 1;
            }
        }
        for (size_t j = i; j > l; --j) {
            if (auto swapped = swap(j - 1, j); !swapped.has_value()) {
                return swapped;
            }
        }
    }
    return {};
}

VoidResult Sorter::insertion_sort(size_t from, size_t to) {
    size_t i = from + 1;
    while (i < to) {
        size_t current = i;
        ++i;
        while (true) {
            const size_t previous = current - 1;
            auto cmp = compare(previous, current);
            if (!cmp.has_value()) {
                return std::unexpected(std::move(cmp.error()));
            }
            if (cmp.value() <= 0) {
                break;
            }
            if (auto swapped = swap(previous, current); !swapped.has_value()) {
                return swapped;
            }
            if (previous == from) {
                break;
            }
            current = previous;
        }
    }
    return {};
}

VoidResult Sorter::heap_sort(size_t from, size_t to) {
    if (to <= from || to - from <= 1) {
        return {};
    }
    if (auto heaped = heapify(from, to); !heaped.has_value()) {
        return heaped;
    }
    size_t end = to - 1;
    while (end > from) {
        if (auto swapped = swap(from, end); !swapped.has_value()) {
            return swapped;
        }
        if (auto sifted = sift_down(from, from, end); !sifted.has_value()) {
            return sifted;
        }
        end -= 1;
    }
    return {};
}

VoidResult Sorter::heapify(size_t from, size_t to) {
    const size_t last_parent = heap_parent(from, to - 1);
    for (size_t i = last_parent + 1; i-- > from;) {
        if (auto sifted = sift_down(i, from, to); !sifted.has_value()) {
            return sifted;
        }
    }
    return {};
}

VoidResult Sorter::sift_down(size_t i, size_t from, size_t to) {
    size_t left_child = heap_child(from, i);
    while (left_child < to) {
        const size_t right_child = left_child + 1;
        auto left_cmp = compare(i, left_child);
        if (!left_cmp.has_value()) {
            return std::unexpected(std::move(left_cmp.error()));
        }
        if (left_cmp.value() < 0) {
            bool swap_with_right = false;
            if (right_child < to) {
                auto right_cmp = compare(left_child, right_child);
                if (!right_cmp.has_value()) {
                    return std::unexpected(std::move(right_cmp.error()));
                }
                swap_with_right = right_cmp.value() < 0;
            }
            const size_t target = swap_with_right ? right_child : left_child;
            if (auto swapped = swap(i, target); !swapped.has_value()) {
                return swapped;
            }
            i = target;
        } else if (right_child < to) {
            auto right_cmp = compare(i, right_child);
            if (!right_cmp.has_value()) {
                return std::unexpected(std::move(right_cmp.error()));
            }
            if (right_cmp.value() < 0) {
                if (auto swapped = swap(i, right_child); !swapped.has_value()) {
                    return swapped;
                }
                i = right_child;
            } else {
                break;
            }
        } else {
            break;
        }
        left_child = heap_child(from, i);
    }
    return {};
}

size_t Sorter::heap_parent(size_t from, size_t i) { return ((i - 1 - from) >> 1) + from; }

size_t Sorter::heap_child(size_t from, size_t i) { return ((i - from) << 1) + 1 + from; }

} // namespace lucene
