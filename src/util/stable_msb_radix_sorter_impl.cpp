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

module lucene_core:stable_msb_radix_sorter.impl;

import :stable_msb_radix_sorter;
import :types;
import :error;
import :msb_radix_sorter;
import :sorter;

import std;

namespace lucene {

namespace {

/// Stable merge sort over a [`StableMSBRadixSorterHooks`] delegate.
///
/// Port of `MergeSorter<MergeSorterImpl<T>>`
/// (`util/stable_msb_radix_sorter.rs`).  Values are staged into the delegate's
/// scratch area and copied back once the merge is done, which is what keeps the
/// sort stable.
class StableMergeSorter final : public Sorter {
public:
    StableMergeSorter(StableMSBRadixSorterHooks &delegate, size_t k, size_t max_length)
        : delegate_(delegate), k_(k), max_length_(max_length) {}

    [[nodiscard]] Result<int32_t> compare(size_t i, size_t j) override {
        for (size_t offset = k_; offset < max_length_; ++offset) {
            auto left = delegate_.byte_at(i, offset);
            if (!left) {
                return std::unexpected(std::move(left.error()));
            }
            auto right = delegate_.byte_at(j, offset);
            if (!right) {
                return std::unexpected(std::move(right.error()));
            }
            if (left.value() != right.value()) {
                return left.value() - right.value();
            }
            if (left.value() == -1) {
                break;
            }
        }
        return 0;
    }

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override { return delegate_.swap(i, j); }

    [[nodiscard]] VoidResult set_pivot(size_t i) override {
        pivot_index_ = i;
        return {};
    }

    [[nodiscard]] Result<int32_t> compare_pivot(size_t j) override { return compare(pivot_index_, j); }

    [[nodiscard]] VoidResult sort(size_t from, size_t to) override {
        if (auto checked = check_range(from, to); !checked) {
            return checked;
        }
        return merge_sort(from, to);
    }

private:
    [[nodiscard]] VoidResult merge_sort(size_t from, size_t to) {
        if (to - from < BINARY_SORT_THRESHOLD) {
            return binary_sort(from, to);
        }
        const size_t mid = (from + to) / 2;
        if (auto r = merge_sort(from, mid); !r) {
            return r;
        }
        if (auto r = merge_sort(mid, to); !r) {
            return r;
        }
        return merge(from, to, mid);
    }

    void bulk_save(size_t from, size_t tmp_from, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            std::ignore = delegate_.save(from + i, tmp_from + i);
        }
    }

    [[nodiscard]] VoidResult merge(size_t from, size_t to, size_t mid) {
        if (to <= mid || mid <= from) {
            return err(LuceneError::illegal_state("invalid merge range"));
        }
        auto adjacent = compare(mid - 1, mid);
        if (!adjacent) {
            return std::unexpected(std::move(adjacent.error()));
        }
        if (adjacent.value() <= 0) {
            return {};
        }

        size_t left = from;
        size_t right = mid;
        size_t index = from;
        while (true) {
            auto compared = compare(left, right);
            if (!compared) {
                return std::unexpected(std::move(compared.error()));
            }
            if (compared.value() <= 0) {
                if (auto r = delegate_.save(left, index); !r) {
                    return r;
                }
                left += 1;
                index += 1;
                if (left == mid) {
                    bulk_save(right, index, to - right);
                    break;
                }
            } else {
                if (auto r = delegate_.save(right, index); !r) {
                    return r;
                }
                right += 1;
                index += 1;
                if (right == to) {
                    bulk_save(left, index, mid - left);
                    break;
                }
            }
        }
        return delegate_.restore(from, to);
    }

    StableMSBRadixSorterHooks &delegate_;
    size_t k_;
    size_t max_length_;
    size_t pivot_index_{0};
};

} // namespace

std::unique_ptr<Sorter> StableMSBRadixSorterHooks::stable_fallback_sorter(size_t k, size_t max_length) {
    std::unique_ptr<Sorter> sorter(new StableMergeSorter(*this, k, max_length));
    return sorter;
}

StableMSBRadixSorter::StableMSBRadixSorter(std::unique_ptr<StableMSBRadixSorterHooks> delegate, size_t max_length)
    : delegate_(std::move(delegate)), max_length_(max_length) {}

std::unique_ptr<Sorter> StableMSBRadixSorter::get_fallback_sorter(size_t k, size_t /*length*/) {
    return delegate_->stable_fallback_sorter(k, max_length_);
}

VoidResult StableMSBRadixSorter::build_histogram(size_t prefix_common_bucket, size_t prefix_common_len, size_t from,
                                                size_t to, size_t k, std::span<size_t> histogram) {
    return delegate_->build_histogram(prefix_common_bucket, prefix_common_len, from, to, k, histogram);
}

VoidResult StableMSBRadixSorter::reorder(size_t from, size_t to, std::span<size_t> start_offsets,
                                        std::span<size_t> end_offsets, size_t k) {
    if (start_offsets.size() < HISTOGRAM_SIZE || end_offsets.size() < HISTOGRAM_SIZE) {
        return err(LuceneError::illegal_argument("radix offsets must have HISTOGRAM_SIZE entries"));
    }
    std::copy_n(start_offsets.begin(), HISTOGRAM_SIZE, fixed_start_offsets_.begin());

    for (size_t i = 0; i < HISTOGRAM_SIZE; ++i) {
        const size_t limit = end_offsets[i];
        size_t h1 = fixed_start_offsets_[i];
        while (h1 < limit) {
            auto bucket = get_bucket(from + h1, k);
            if (!bucket) {
                return std::unexpected(std::move(bucket.error()));
            }
            const size_t h2 = start_offsets[bucket.value()];
            start_offsets[bucket.value()] += 1;
            if (auto r = delegate_->save(from + h1, from + h2); !r) {
                return r;
            }
            h1 += 1;
        }
    }
    return delegate_->restore(from, to);
}

} // namespace lucene
