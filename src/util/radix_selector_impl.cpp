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

module lucene_core:radix_selector.impl;

import :radix_selector;
import :types;
import :error;
import :intro_selector;
import :selector;

import std;

namespace lucene {

namespace {

/// Introselect over the bytes from offset `d` on, used once the bucket is small
/// or the recursion is deep.
///
/// Port of `IntroSelectorImpl` (`util/radix_selector.rs`).
class RadixFallbackHooks final : public IntroSelectorHooks {
public:
    RadixFallbackHooks(RadixSelectorHooks &delegate, size_t d, size_t max_length)
        : delegate_(delegate), d_(d), max_length_(max_length) {}

    [[nodiscard]] VoidResult set_pivot(size_t i) override {
        pivot_.clear();
        for (size_t offset = d_; offset < max_length_; ++offset) {
            auto byte = delegate_.byte_at(i, offset);
            if (!byte) {
                return std::unexpected(std::move(byte.error()));
            }
            if (byte.value() == -1) {
                break;
            }
            pivot_.push_back(static_cast<uint8_t>(byte.value()));
        }
        return {};
    }

    [[nodiscard]] Result<int32_t> compare_pivot(size_t j) override {
        for (size_t offset = 0; offset < pivot_.size(); ++offset) {
            const int32_t left = pivot_[offset];
            auto right = delegate_.byte_at(j, d_ + offset);
            if (!right) {
                return std::unexpected(std::move(right.error()));
            }
            if (left != right.value()) {
                return left - right.value();
            }
        }
        if (d_ + pivot_.size() == max_length_) {
            return 0;
        }
        auto byte = delegate_.byte_at(j, d_ + pivot_.size());
        if (!byte) {
            return std::unexpected(std::move(byte.error()));
        }
        return -1 - byte.value();
    }

    [[nodiscard]] Result<int32_t> compare(size_t i, size_t j) override {
        for (size_t offset = d_; offset < max_length_; ++offset) {
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

private:
    RadixSelectorHooks &delegate_;
    size_t d_;
    size_t max_length_;
    std::vector<uint8_t> pivot_;
};

} // namespace

RadixSelector::RadixSelector(size_t max_length, RadixSelectorHooks &hooks)
    : max_length_(max_length), common_prefix_(std::min<size_t>(24, max_length)), hooks_(hooks) {}

VoidResult RadixSelector::select(size_t from, size_t to, size_t k) {
    if (auto checked = check_args(from, to, k); !checked) {
        return checked;
    }
    return select_impl(from, to, k, 0, 0);
}

VoidResult RadixSelector::select_impl(size_t from, size_t to, size_t k, size_t d, size_t level) {
    if (to - from <= LENGTH_THRESHOLD || level >= LEVEL_THRESHOLD) {
        RadixFallbackHooks fallback(hooks_, d, max_length_);
        IntroSelector selector(fallback);
        return selector.select(from, to, k);
    }
    return radix_select(from, to, k, d, level);
}

Result<size_t> RadixSelector::get_bucket(size_t i, size_t k) {
    auto byte = hooks_.byte_at(i, k);
    if (!byte) {
        return std::unexpected(std::move(byte.error()));
    }
    if (byte.value() < -1 || byte.value() > 255) {
        return err(LuceneError::illegal_argument(
            std::format("byte_at() must return -1 or a byte, got {}", byte.value())));
    }
    return static_cast<size_t>(byte.value() + 1);
}

VoidResult RadixSelector::radix_select(size_t from, size_t to, size_t k, size_t d, size_t level) {
    histogram_.fill(0);

    auto prefix = compute_common_prefix_length_and_build_histogram(from, to, d);
    if (!prefix) {
        return std::unexpected(std::move(prefix.error()));
    }
    const size_t common_prefix_length = prefix.value();
    if (common_prefix_length > 0) {
        // When all values end before `d`, or share the whole prefix, the bucket
        // is already selected; otherwise skip the common prefix in one step.
        if (d + common_prefix_length < max_length_ && histogram_[0] < to - from) {
            return radix_select(from, to, k, d + common_prefix_length, level);
        }
        return {};
    }

    size_t bucket_from = from;
    for (size_t bucket = 0; bucket < RADIX_HISTOGRAM_SIZE; ++bucket) {
        const size_t bucket_to = bucket_from + histogram_[bucket];
        if (bucket_to > k) {
            if (auto r = partition(from, to, bucket, bucket_from, bucket_to, d); !r) {
                return r;
            }
            if (bucket != 0 && d + 1 < max_length_) {
                // Bucket 0 holds the values that ended here, which are all
                // equal, so only other buckets need to recurse.
                return select_impl(bucket_from, bucket_to, k, d + 1, level + 1);
            }
            return {};
        }
        bucket_from = bucket_to;
    }
    return err(LuceneError::illegal_state("radix select histogram does not cover k"));
}

Result<size_t> RadixSelector::compute_initial_common_prefix_length(size_t from, size_t k) {
    size_t common_prefix_length = std::min(common_prefix_.size(), max_length_ - k);
    for (size_t j = 0; j < common_prefix_length; ++j) {
        auto byte = hooks_.byte_at(from, k + j);
        if (!byte) {
            return std::unexpected(std::move(byte.error()));
        }
        common_prefix_[j] = byte.value();
        if (byte.value() == -1) {
            common_prefix_length = j + 1;
            break;
        }
    }
    return common_prefix_length;
}

Result<size_t> RadixSelector::compute_common_prefix_length_and_build_histogram(size_t from,
                                                                              size_t to,
                                                                              size_t k) {
    auto initial = compute_initial_common_prefix_length(from, k);
    if (!initial) {
        return std::unexpected(std::move(initial.error()));
    }
    size_t common_prefix_length = initial.value();

    size_t i = from + 1;
    bool broke_outer = false;
    for (size_t current = from + 1; current <= to; ++current) {
        i = current;
        if (i == to) {
            break;
        }
        for (size_t j = 0; j < common_prefix_length; ++j) {
            auto byte = hooks_.byte_at(current, k + j);
            if (!byte) {
                return std::unexpected(std::move(byte.error()));
            }
            if (byte.value() != common_prefix_[j]) {
                common_prefix_length = j;
                if (common_prefix_length == 0) {
                    histogram_[static_cast<size_t>(common_prefix_[0] + 1)] = i - from;
                    histogram_[static_cast<size_t>(byte.value() + 1)] = 1;
                    broke_outer = true;
                }
                break;
            }
        }
        if (broke_outer) {
            break;
        }
    }

    if (i < to) {
        // The loop broke because there is no common prefix left.
        if (auto r = build_histogram(i + 1, to, k); !r) {
            return std::unexpected(std::move(r.error()));
        }
    } else {
        histogram_[static_cast<size_t>(common_prefix_[0] + 1)] = to - from;
    }
    return common_prefix_length;
}

VoidResult RadixSelector::build_histogram(size_t from, size_t to, size_t k) {
    for (size_t i = from; i < to; ++i) {
        auto bucket = get_bucket(i, k);
        if (!bucket) {
            return std::unexpected(std::move(bucket.error()));
        }
        histogram_[bucket.value()] += 1;
    }
    return {};
}

VoidResult RadixSelector::partition(size_t from,
                                    size_t to,
                                    size_t bucket,
                                    size_t bucket_from,
                                    size_t bucket_to,
                                    size_t d) {
    size_t left = from;
    size_t right = to - 1;
    size_t slot = bucket_from;
    while (true) {
        auto left_bucket = get_bucket(left, d);
        if (!left_bucket) {
            return std::unexpected(std::move(left_bucket.error()));
        }
        auto right_bucket = get_bucket(right, d);
        if (!right_bucket) {
            return std::unexpected(std::move(right_bucket.error()));
        }

        while (left_bucket.value() <= bucket && left < bucket_from) {
            if (left_bucket.value() == bucket) {
                if (auto r = hooks_.swap(left, slot); !r) {
                    return r;
                }
                slot += 1;
            } else {
                left += 1;
            }
            auto next = get_bucket(left, d);
            if (!next) {
                return std::unexpected(std::move(next.error()));
            }
            left_bucket = next.value();
        }

        while (right_bucket.value() >= bucket && right >= bucket_to) {
            if (right_bucket.value() == bucket) {
                if (auto r = hooks_.swap(right, slot); !r) {
                    return r;
                }
                slot += 1;
            } else {
                right -= 1;
            }
            auto next = get_bucket(right, d);
            if (!next) {
                return std::unexpected(std::move(next.error()));
            }
            right_bucket = next.value();
        }

        if (left < bucket_from && right >= bucket_to) {
            if (auto r = hooks_.swap(left, right); !r) {
                return r;
            }
            left += 1;
            right -= 1;
        } else {
            break;
        }
    }
    return {};
}

} // namespace lucene
