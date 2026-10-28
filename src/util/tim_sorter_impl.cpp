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

module lucene_core:tim_sorter.impl;

import :tim_sorter;
import :types;
import :error;
import :sorter;

import std;

namespace lucene {

size_t tim_min_run(size_t length) {
    size_t n = length;
    size_t r = 0;
    while (n >= 64) {
        r |= n & 1;
        n >>= 1;
    }
    const size_t result = n + r;
    return std::clamp(result, TIM_MIN_RUN, TIM_THRESHOLD);
}

TimSorter::TimSorter(size_t max_temp_slots, TimSorterHooks &delegate)
    : max_temp_slots_(max_temp_slots), delegate_(delegate) {}

void TimSorter::reset(size_t from, size_t to) {
    stack_size_ = 0;
    run_ends_.fill(0);
    run_ends_[0] = from;
    to_ = to;
    const size_t length = to - from;
    min_run_ = length <= TIM_THRESHOLD ? length : tim_min_run(length);
}

void TimSorter::push_run_len(size_t len) {
    run_ends_[stack_size_ + 1] = run_ends_[stack_size_] + len;
    stack_size_ += 1;
}

VoidResult TimSorter::sort(size_t from, size_t to) {
    if (auto checked = check_range(from, to); !checked) {
        return checked;
    }
    if (to - from <= 1) {
        return {};
    }

    reset(from, to);

    while (true) {
        if (auto r = ensure_invariants(); !r) {
            return r;
        }
        auto run_length = next_run();
        if (!run_length) {
            return std::unexpected(std::move(run_length.error()));
        }
        push_run_len(run_length.value());
        if (run_end(0) >= to) {
            break;
        }
    }
    return exhaust_stack();
}

Result<size_t> TimSorter::next_run() {
    const size_t base = run_end(0);

    if (base == to_ - 1) {
        return size_t{1};
    }
    size_t o = base + 2;
    auto first = compare(base, base + 1);
    if (!first) {
        return std::unexpected(std::move(first.error()));
    }
    if (first.value() > 0) {
        while (o < to_) {
            auto compared = compare(o - 1, o);
            if (!compared) {
                return std::unexpected(std::move(compared.error()));
            }
            if (compared.value() <= 0) {
                break;
            }
            o += 1;
        }
        if (auto r = reverse(base, o); !r) {
            return std::unexpected(std::move(r.error()));
        }
    } else {
        while (o < to_) {
            auto compared = compare(o - 1, o);
            if (!compared) {
                return std::unexpected(std::move(compared.error()));
            }
            if (compared.value() > 0) {
                break;
            }
            o += 1;
        }
    }
    const size_t run_hi = std::max(o, std::min(to_, base + min_run_));
    if (auto r = binary_sort_with_start(base, run_hi, o); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return run_hi - base;
}

VoidResult TimSorter::ensure_invariants() {
    while (stack_size_ > 1) {
        const size_t run_len0 = run_len(0);
        const size_t run_len1 = run_len(1);

        if (stack_size_ > 2) {
            const size_t run_len2 = run_len(2);
            if (run_len2 <= run_len1 + run_len0) {
                // Merge the smaller of runs 0 and 2 with run 1.
                if (auto r = merge_at(run_len2 < run_len0 ? 1 : 0); !r) {
                    return r;
                }
                continue;
            }
        }

        if (run_len1 <= run_len0) {
            if (auto r = merge_at(0); !r) {
                return r;
            }
            continue;
        }
        break;
    }
    return {};
}

VoidResult TimSorter::exhaust_stack() {
    while (stack_size_ > 1) {
        if (auto r = merge_at(0); !r) {
            return r;
        }
    }
    return {};
}

VoidResult TimSorter::merge_at(size_t n) {
    if (stack_size_ < 2) {
        return err(LuceneError::illegal_state("a merge needs at least two runs"));
    }
    if (auto r = merge(run_base(n + 1), run_base(n), run_end(n)); !r) {
        return r;
    }
    for (size_t j = n + 1; j >= 1; --j) {
        set_run_end(j, run_end(j - 1));
    }
    stack_size_ -= 1;
    return {};
}

VoidResult TimSorter::merge(size_t lo, size_t mid, size_t hi) {
    auto adjacent = compare(mid - 1, mid);
    if (!adjacent) {
        return std::unexpected(std::move(adjacent.error()));
    }
    if (adjacent.value() <= 0) {
        return {};
    }

    auto new_lo = upper2(lo, mid, mid);
    if (!new_lo) {
        return std::unexpected(std::move(new_lo.error()));
    }
    lo = new_lo.value();
    auto new_hi = lower2(mid, hi, mid - 1);
    if (!new_hi) {
        return std::unexpected(std::move(new_hi.error()));
    }
    hi = new_hi.value();

    if (hi - mid <= mid - lo && hi - mid <= max_temp_slots_) {
        return merge_hi(lo, mid, hi);
    }
    if (mid - lo <= max_temp_slots_) {
        return merge_lo(lo, mid, hi);
    }
    return merge_in_place(lo, mid, hi);
}

VoidResult TimSorter::merge_lo(size_t lo, size_t mid, size_t hi) {
    const size_t len1 = mid - lo;
    if (auto r = delegate_.save(lo, len1); !r) {
        return r;
    }
    if (auto r = delegate_.copy(mid, lo); !r) {
        return r;
    }

    size_t i = 0;
    size_t j = mid + 1;
    size_t dest = lo + 1;

    bool exhausted = false;
    while (!exhausted) {
        size_t count = 0;
        while (count < TIM_MIN_GALLOP) {
            if (i >= len1 || j >= hi) {
                exhausted = true;
                break;
            }
            auto compared = delegate_.compare_saved(i, j);
            if (!compared) {
                return std::unexpected(std::move(compared.error()));
            }
            if (compared.value() <= 0) {
                delegate_.restore(i, dest);
                i += 1;
                dest += 1;
                count = 0;
            } else {
                if (auto r = delegate_.copy(j, dest); !r) {
                    return r;
                }
                j += 1;
                dest += 1;
                count += 1;
            }
        }
        if (exhausted) {
            break;
        }

        // Galloping phase: copy everything that sorts before the saved value.
        auto next = lower_saved3(j, hi, i);
        if (!next) {
            return std::unexpected(std::move(next.error()));
        }
        while (j < next.value()) {
            if (auto r = delegate_.copy(j, dest); !r) {
                return r;
            }
            j += 1;
            dest += 1;
        }
        delegate_.restore(i, dest);
        i += 1;
        dest += 1;
    }

    while (i < len1) {
        delegate_.restore(i, dest);
        i += 1;
        dest += 1;
    }
    return {};
}

VoidResult TimSorter::merge_hi(size_t lo, size_t mid, size_t hi) {
    const size_t len2 = hi - mid;
    if (auto r = delegate_.save(mid, len2); !r) {
        return r;
    }
    if (auto r = delegate_.copy(mid - 1, hi - 1); !r) {
        return r;
    }

    // Exclusive cursors, so exhaustion never needs to step below zero.
    size_t i = mid - 1;
    size_t j = len2;
    size_t dest = hi - 1;

    bool exhausted = false;
    while (!exhausted) {
        size_t count = 0;
        while (count < TIM_MIN_GALLOP) {
            if (i <= lo || j == 0) {
                exhausted = true;
                break;
            }
            auto compared = delegate_.compare_saved(j - 1, i - 1);
            if (!compared) {
                return std::unexpected(std::move(compared.error()));
            }
            if (compared.value() >= 0) {
                delegate_.restore(j - 1, dest - 1);
                j -= 1;
                dest -= 1;
                count = 0;
            } else {
                if (auto r = delegate_.copy(i - 1, dest - 1); !r) {
                    return r;
                }
                i -= 1;
                dest -= 1;
                count += 1;
            }
        }
        if (exhausted) {
            break;
        }

        auto next = upper_saved3(lo, i, j - 1);
        if (!next) {
            return std::unexpected(std::move(next.error()));
        }
        while (i > next.value()) {
            if (auto r = delegate_.copy(i - 1, dest - 1); !r) {
                return r;
            }
            i -= 1;
            dest -= 1;
        }
        delegate_.restore(j - 1, dest - 1);
        j -= 1;
        dest -= 1;
    }

    while (j > 0) {
        delegate_.restore(j - 1, dest - 1);
        j -= 1;
        dest -= 1;
    }
    return {};
}

Result<size_t> TimSorter::lower_saved(size_t from, size_t to, size_t val) const {
    size_t begin = from;
    if (to < begin) {
        return begin;
    }
    size_t len = to - begin;

    while (len > 0) {
        const size_t half = len >> 1;
        const size_t mid = begin + half;
        auto compared = delegate_.compare_saved(val, mid);
        if (!compared) {
            return std::unexpected(std::move(compared.error()));
        }
        if (compared.value() > 0) {
            begin = mid + 1;
            len -= half + 1;
        } else {
            len = half;
        }
    }
    return begin;
}

Result<size_t> TimSorter::upper_saved(size_t from, size_t to, size_t val) const {
    size_t begin = from;
    if (to < begin) {
        return begin;
    }
    size_t len = to - begin;

    while (len > 0) {
        const size_t half = len >> 1;
        const size_t mid = begin + half;
        auto compared = delegate_.compare_saved(val, mid);
        if (!compared) {
            return std::unexpected(std::move(compared.error()));
        }
        if (compared.value() < 0) {
            len = half;
        } else {
            begin = mid + 1;
            len -= half + 1;
        }
    }
    return begin;
}

Result<size_t> TimSorter::lower_saved3(size_t from, size_t to, size_t val) const {
    size_t f = from;
    size_t t = f + 1;

    while (t < to) {
        auto compared = delegate_.compare_saved(val, t);
        if (!compared) {
            return std::unexpected(std::move(compared.error()));
        }
        if (compared.value() <= 0) {
            return lower_saved(f, t, val);
        }
        const size_t delta = t - f;
        f = t;
        t += delta * 2;
    }
    return lower_saved(f, to, val);
}

Result<size_t> TimSorter::upper_saved3(size_t from, size_t to, size_t val) const {
    size_t f = to > 0 ? to - 1 : 0;
    size_t t = to;

    while (f > from) {
        auto compared = delegate_.compare_saved(val, f);
        if (!compared) {
            return std::unexpected(std::move(compared.error()));
        }
        if (compared.value() >= 0) {
            return upper_saved(f, t, val);
        }
        const size_t delta = t - f;
        t = f;
        // A probe below `from` ends the gallop.
        const size_t step = delta * 2;
        f = f > step ? f - step : 0;
    }
    return upper_saved(from, t, val);
}

VoidResult TimSorter::do_rotate(size_t lo, size_t mid, size_t hi) {
    size_t left = lo;
    size_t middle = mid;
    const size_t len1 = mid - lo;
    const size_t len2 = hi - mid;

    if (len1 == len2) {
        while (middle < hi) {
            if (auto r = swap(left, middle); !r) {
                return r;
            }
            left += 1;
            middle += 1;
        }
    } else if (len2 < len1 && len2 <= max_temp_slots_) {
        if (auto r = delegate_.save(mid, len2); !r) {
            return r;
        }
        for (size_t i = middle; i > lo; --i) {
            if (auto r = delegate_.copy(i - 1, i - 1 + len2); !r) {
                return r;
            }
        }
        for (size_t i = 0; i < len2; ++i) {
            delegate_.restore(i, lo + i);
        }
    } else if (len1 <= max_temp_slots_) {
        if (auto r = delegate_.save(lo, len1); !r) {
            return r;
        }
        size_t i = middle;
        size_t j = lo;
        while (i < hi) {
            if (auto r = delegate_.copy(i, j); !r) {
                return r;
            }
            i += 1;
            j += 1;
        }
        i = 0;
        j = lo + len2;
        while (j < hi) {
            delegate_.restore(i, j);
            i += 1;
            j += 1;
        }
    } else {
        if (auto r = reverse(lo, mid); !r) {
            return r;
        }
        if (auto r = reverse(mid, hi); !r) {
            return r;
        }
        if (auto r = reverse(lo, hi); !r) {
            return r;
        }
    }
    return {};
}

} // namespace lucene
