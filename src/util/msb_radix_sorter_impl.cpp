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

module lucene_core:msb_radix_sorter.impl;

import :msb_radix_sorter;

import :debug;
import :error;
import :intro_sorter;
import :sorter;
import :types;

import std;

namespace lucene {

namespace {

/// The introsort the radix sorter falls back to: compares keys byte by byte
/// from `k` on, using the delegate's `byte_at`.
///
/// Port of `MSBRadixIntroSorterImpl` (`util/most_significant_bit_radix_sort.rs`).
class MSBRadixIntroSorter final : public IntroSorter {
public:
    MSBRadixIntroSorter(MSBRadixSorterBase &delegate, size_t k, size_t max_length)
            : delegate_(delegate), k_(k), max_length_(max_length) {}

    [[nodiscard]] Result<int32_t> compare(size_t i, size_t j) override {
        for (size_t o = k_; o < max_length_; ++o) {
            auto b1 = delegate_.byte_at(i, o);
            if (!b1.has_value()) {
                return std::unexpected(std::move(b1.error()));
            }
            auto b2 = delegate_.byte_at(j, o);
            if (!b2.has_value()) {
                return std::unexpected(std::move(b2.error()));
            }
            if (b1.value() != b2.value()) {
                return b1.value() - b2.value();
            }
            if (b1.value() == -1) {
                break;
            }
        }
        return 0;
    }

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override { return delegate_.swap(i, j); }

    [[nodiscard]] VoidResult set_pivot(size_t i) override {
        pivot_.clear();
        for (size_t o = k_; o < max_length_; ++o) {
            auto b = delegate_.byte_at(i, o);
            if (!b.has_value()) {
                return std::unexpected(std::move(b.error()));
            }
            if (b.value() == -1) {
                break;
            }
            pivot_.push_back(static_cast<uint8_t>(b.value()));
        }
        return {};
    }

    [[nodiscard]] Result<int32_t> compare_pivot(size_t j) override {
        for (size_t o = 0; o < pivot_.size(); ++o) {
            const int32_t b1 = pivot_[o];
            auto b2 = delegate_.byte_at(j, k_ + o);
            if (!b2.has_value()) {
                return std::unexpected(std::move(b2.error()));
            }
            if (b1 != b2.value()) {
                return b1 - b2.value();
            }
        }
        if (k_ + pivot_.size() == max_length_) {
            return 0;
        }
        auto next = delegate_.byte_at(j, k_ + pivot_.size());
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        return -1 - next.value();
    }

private:
    MSBRadixSorterBase &delegate_;
    size_t k_;
    size_t max_length_;
    std::vector<uint8_t> pivot_;
};

} // namespace

// ---------------------------------------------------------------------------
// MSBRadixSorterBase
// ---------------------------------------------------------------------------

Result<int32_t> MSBRadixSorterBase::byte_at(size_t /*i*/, size_t /*k*/) {
    return err(LuceneError::not_implemented("byte_at"));
}

std::unique_ptr<Sorter> MSBRadixSorterBase::get_fallback_sorter(size_t k, size_t length) {
    return std::make_unique<MSBRadixIntroSorter>(*this, k, length);
}

VoidResult MSBRadixSorterBase::reorder(size_t from, size_t /*to*/, std::span<size_t> start_offsets,
                                       std::span<size_t> end_offsets, size_t k) {
    // Reorder in place, similar to the Dutch national flag problem.
    for (size_t i = 0; i < HISTOGRAM_SIZE; ++i) {
        const size_t limit = end_offsets[i];
        while (start_offsets[i] < limit) {
            const size_t h1 = start_offsets[i];
            auto bucket = get_bucket(from + h1, k);
            if (!bucket.has_value()) {
                return std::unexpected(std::move(bucket.error()));
            }
            const size_t h2 = start_offsets[bucket.value()];
            start_offsets[bucket.value()] += 1;
            if (auto swapped = swap(from + h1, from + h2); !swapped.has_value()) {
                return swapped;
            }
        }
    }
    return {};
}

Result<size_t> MSBRadixSorterBase::get_bucket(size_t i, size_t k) {
    auto b = byte_at(i, k);
    if (!b.has_value()) {
        return std::unexpected(std::move(b.error()));
    }
    return static_cast<size_t>(b.value() + 1);
}

VoidResult MSBRadixSorterBase::build_histogram(size_t prefix_common_bucket, size_t prefix_common_len, size_t from,
                                               size_t to, size_t k, std::span<size_t> histogram) {
    histogram[prefix_common_bucket] = prefix_common_len;
    for (size_t i = from; i < to; ++i) {
        auto bucket = get_bucket(i, k);
        if (!bucket.has_value()) {
            return std::unexpected(std::move(bucket.error()));
        }
        histogram[bucket.value()] += 1;
    }
    return {};
}

bool MSBRadixSorterBase::should_fallback(size_t from, size_t to, size_t level) {
    return (to - from) <= LENGTH_THRESHOLD || level >= LEVEL_THRESHOLD;
}

// ---------------------------------------------------------------------------
// MSBRadixSorter
// ---------------------------------------------------------------------------

MSBRadixSorter::MSBRadixSorter(size_t max_length, std::unique_ptr<MSBRadixSorterBase> delegate)
        : delegate_(std::move(delegate)), max_length_(max_length) {}

VoidResult MSBRadixSorter::sort(size_t from, size_t to) {
    if (auto checked = check_range(from, to); !checked.has_value()) {
        return checked;
    }
    return sort_impl(from, to, 0, 0);
}

VoidResult MSBRadixSorter::sort_impl(size_t from, size_t to, size_t k, size_t level) {
    if (should_fallback(from, to, level)) {
        auto fallback = delegate_->get_fallback_sorter(k, max_length_);
        return fallback->sort(from, to);
    }
    return radix_sort(from, to, k, level);
}

bool MSBRadixSorter::should_fallback(size_t from, size_t to, size_t level) {
    return delegate_->should_fallback(from, to, level);
}

Result<size_t> MSBRadixSorter::compute_initial_common_prefix_length(size_t from, size_t k) {
    const size_t slots = std::min<size_t>(24, max_length_);
    size_t common_prefix_length = std::min(slots, max_length_ - k);

    for (size_t j = 0; j < common_prefix_length; ++j) {
        auto b = delegate_->byte_at(from, k + j);
        if (!b.has_value()) {
            return std::unexpected(std::move(b.error()));
        }
        common_prefix_[j] = b.value();
        if (b.value() == -1) {
            common_prefix_length = j + 1;
            break;
        }
    }
    return common_prefix_length;
}

Result<size_t> MSBRadixSorter::compute_common_prefix_length_and_build_histogram_part1(size_t from, size_t to,
                                                                                     size_t k, size_t level,
                                                                                     size_t common_prefix_length) {
    size_t i = from + 1;

    bool abort_range = false;
    for (size_t idx = from + 1; idx < to && !abort_range; ++idx) {
        size_t j = 0;
        while (j < common_prefix_length) {
            auto b = delegate_->byte_at(idx, k + j);
            if (!b.has_value()) {
                return std::unexpected(std::move(b.error()));
            }
            if (b.value() != common_prefix_[j]) {
                common_prefix_length = j;
                if (common_prefix_length == 0) {
                    abort_range = true;
                }
                break;
            }
            j += 1;
        }
        if (abort_range) {
            break;
        }
        i = idx + 1;
    }

    return compute_common_prefix_length_and_build_histogram_part2(from, to, k, level, common_prefix_length, i);
}

Result<size_t> MSBRadixSorter::compute_common_prefix_length_and_build_histogram_part2(size_t from, size_t to,
                                                                                     size_t k, size_t level,
                                                                                     size_t common_prefix_length,
                                                                                     size_t i) {
    const auto prefix_common_bucket = static_cast<size_t>(common_prefix_[0] + 1);
    if (i < to) {
        debug_check(common_prefix_length == 0, "a shared prefix means every entry matched");
        if (auto built = delegate_->build_histogram(prefix_common_bucket, i - from, i, to, k, histograms_[level]);
            !built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
    } else {
        debug_check(common_prefix_length > 0, "a full match means there is a shared prefix");
        histograms_[level][prefix_common_bucket] = to - from;
    }
    return common_prefix_length;
}

Result<size_t> MSBRadixSorter::compute_common_prefix_length_and_build_histogram(size_t from, size_t to, size_t k,
                                                                               size_t level) {
    auto initial = compute_initial_common_prefix_length(from, k);
    if (!initial.has_value()) {
        return std::unexpected(std::move(initial.error()));
    }
    return compute_common_prefix_length_and_build_histogram_part1(from, to, k, level, initial.value());
}

void MSBRadixSorter::sum_histogram(std::span<size_t> histogram, std::span<size_t> end_offsets) {
    size_t accum = 0;
    for (size_t i = 0; i < histogram.size(); ++i) {
        const size_t count = histogram[i];
        histogram[i] = accum;
        accum += count;
        end_offsets[i] = accum;
    }
}

VoidResult MSBRadixSorter::reorder(size_t from, size_t to, size_t level, size_t k) {
    return delegate_->reorder(from, to, histograms_[level], end_offsets_, k);
}

VoidResult MSBRadixSorter::radix_sort(size_t from, size_t to, size_t k, size_t level) {
    auto &histogram = histograms_[level];
    if (histogram.empty()) {
        histogram.assign(HISTOGRAM_SIZE, 0);
    } else {
        std::fill(histogram.begin(), histogram.end(), size_t{0});
    }

    auto computed = compute_common_prefix_length_and_build_histogram(from, to, k, level);
    if (!computed.has_value()) {
        return std::unexpected(std::move(computed.error()));
    }
    const size_t common_prefix_length = computed.value();

    if (common_prefix_length > 0) {
        // Either there is nothing left to compare, or every entry fell into the
        // first bucket (the keys are shorter than `k`): we are done.  Otherwise
        // skip the shared prefix and recurse.
        if (k + common_prefix_length < max_length_ && histogram[0] < (to - from)) {
            return radix_sort(from, to, k + common_prefix_length, level);
        }
        return {};
    }

    sum_histogram(histogram, end_offsets_);
    if (auto reordered = reorder(from, to, level, k); !reordered.has_value()) {
        return reordered;
    }

    if (k + 1 < max_length_) {
        size_t prev = histogram[0];
        for (size_t i = 1; i < HISTOGRAM_SIZE; ++i) {
            const size_t h = histogram[i];
            if (h - prev > 1) {
                if (auto sorted = sort_impl(from + prev, from + h, k + 1, level + 1); !sorted.has_value()) {
                    return sorted;
                }
            }
            prev = h;
        }
    }
    return {};
}

} // namespace lucene
