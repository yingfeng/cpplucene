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

module lucene_core:paged_mutable.impl;

import :paged_mutable;

import :error;
import :packed64;
import :packed_ints;
import :types;

import std;

namespace lucene {

namespace {

/// Values per page: the reference implementation's `PAGE_SIZE` scaled by the
/// width, so a page holds about 512 bytes (`PackedInts::DEFAULT_BUFFER_SIZE / 2`).
[[nodiscard]] size_t page_size_for(int32_t bits_per_value) {
    const size_t bits = static_cast<size_t>(bits_per_value < 1 ? 1 : bits_per_value);
    const size_t values = (static_cast<size_t>(PackedInts::DEFAULT_BUFFER_SIZE) * 8) / bits;
    return values == 0 ? 1 : values;
}

} // namespace

// ---- PagedMutable ----

Result<PagedMutable> PagedMutable::create(size_t size, int32_t bits_per_value, float acceptable_overhead_ratio) {
    static_cast<void>(acceptable_overhead_ratio);
    if (bits_per_value < 0 || bits_per_value > 64) {
        return err(LuceneError::illegal_argument("bits_per_value must be in [0, 64], got " +
                                                 std::to_string(bits_per_value)));
    }
    const size_t page_size = page_size_for(bits_per_value);
    const size_t page_count = size == 0 ? 0 : (size + page_size - 1) / page_size;
    std::vector<Packed64> pages;
    pages.reserve(page_count);
    for (size_t page = 0; page < page_count; ++page) {
        const size_t remaining = size - page * page_size;
        auto created = Packed64::create(std::min(page_size, remaining), bits_per_value);
        if (!created.has_value()) {
            return std::unexpected(std::move(created.error()));
        }
        pages.push_back(std::move(created.value()));
    }
    return PagedMutable(page_size, bits_per_value, size, std::move(pages));
}

int64_t PagedMutable::get(size_t index) const {
    if (index >= size_) {
        return std::numeric_limits<int64_t>::min();
    }
    return pages_[index / page_size_].get(index % page_size_);
}

VoidResult PagedMutable::set(size_t index, int64_t value) {
    if (index >= size_) {
        return err(LuceneError::illegal_argument("index " + std::to_string(index) + " is out of bounds for size " +
                                                 std::to_string(size_)));
    }
    return pages_[index / page_size_].set(index % page_size_, value);
}

Result<PagedMutable> PagedMutable::resize(size_t new_size) const {
    auto grown = create(new_size, bits_per_value_);
    if (!grown.has_value()) {
        return std::unexpected(std::move(grown.error()));
    }
    PagedMutable result = std::move(grown.value());
    const size_t kept = std::min(size_, new_size);
    for (size_t i = 0; i < kept; ++i) {
        if (auto set_value = result.set(i, get(i)); !set_value.has_value()) {
            return std::unexpected(std::move(set_value.error()));
        }
    }
    return result;
}

VoidResult PagedMutable::fill(size_t from, size_t to, int64_t value) {
    if (from > to || to > size_) {
        return err(LuceneError::illegal_argument("cannot fill [" + std::to_string(from) + ", " +
                                                 std::to_string(to) + ") of an array of size " +
                                                 std::to_string(size_)));
    }
    for (size_t i = from; i < to; ++i) {
        if (auto set_value = set(i, value); !set_value.has_value()) {
            return set_value;
        }
    }
    return {};
}

int64_t PagedMutable::ram_bytes_used() const {
    int64_t bytes = static_cast<int64_t>(sizeof(PagedMutable) + pages_.size() * sizeof(Packed64));
    for (const Packed64 &page : pages_) {
        bytes += static_cast<int64_t>(page.ram_bytes_used());
    }
    return bytes;
}

// ---- PagedGrowableWriter ----

Result<PagedGrowableWriter> PagedGrowableWriter::create(size_t size, int32_t start_bits_per_value,
                                                        float acceptable_overhead_ratio) {
    if (start_bits_per_value < 1 || start_bits_per_value > 64) {
        return err(LuceneError::illegal_argument("start_bits_per_value must be in [1, 64], got " +
                                                 std::to_string(start_bits_per_value)));
    }
    auto values = PagedMutable::create(size, start_bits_per_value, acceptable_overhead_ratio);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    return PagedGrowableWriter(std::move(values.value()), acceptable_overhead_ratio);
}

VoidResult PagedGrowableWriter::set(size_t index, int64_t value) {
    if (value < 0) {
        return err(LuceneError::illegal_argument("a packed value cannot be negative, got " +
                                                 std::to_string(value)));
    }
    if (value > PackedInts::max_value(values_.bits_per_value())) {
        // Widen the layout: one more bit per value than the value needs.
        const int32_t needed = PackedInts::unsigned_bits_required(value);
        auto widened = values_.resize(values_.size());
        if (!widened.has_value()) {
            return std::unexpected(std::move(widened.error()));
        }
        auto repaged = PagedMutable::create(values_.size(), needed, acceptable_overhead_ratio_);
        if (!repaged.has_value()) {
            return std::unexpected(std::move(repaged.error()));
        }
        PagedMutable next = std::move(repaged.value());
        for (size_t i = 0; i < values_.size(); ++i) {
            if (auto copied = next.set(i, values_.get(i)); !copied.has_value()) {
                return copied;
            }
        }
        values_ = std::move(next);
    }
    return values_.set(index, value);
}

Result<PagedGrowableWriter> PagedGrowableWriter::resize(size_t new_size) const {
    auto resized = values_.resize(new_size);
    if (!resized.has_value()) {
        return std::unexpected(std::move(resized.error()));
    }
    return PagedGrowableWriter(std::move(resized.value()), acceptable_overhead_ratio_);
}

} // namespace lucene
