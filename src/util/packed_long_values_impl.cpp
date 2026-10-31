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

module lucene_core:packed_long_values.impl;

import :packed_long_values;

import :debug;
import :error;
import :packed64;
import :types;

import std;

namespace lucene {

namespace {

/// `left + right`, wrapping like the reference implementation's `wrapping_add`.
///
/// A page may legitimately span more than `INT64_MAX` (`INT64_MIN` round-trips
/// through the packed values, and a delta page then adds its minimum back), and
/// signed overflow is undefined behaviour in C++, so the arithmetic goes through
/// `uint64_t` where the wrap is well defined.
[[nodiscard]] int64_t wrapping_add(int64_t left, int64_t right) noexcept {
    return static_cast<int64_t>(static_cast<uint64_t>(left) + static_cast<uint64_t>(right));
}

/// `left - right`, wrapping like the reference implementation's `wrapping_sub`.
[[nodiscard]] int64_t wrapping_sub(int64_t left, int64_t right) noexcept {
    return static_cast<int64_t>(static_cast<uint64_t>(left) - static_cast<uint64_t>(right));
}

[[nodiscard]] Result<int32_t> bits_required(int64_t max_value) {
    if (max_value < 0) {
        // Values are packed as unsigned bit patterns, so a value whose top bit is
        // set (a negative delta, or a raw double bit pattern) needs all 64 bits.
        // Wrapping the page delta can land here, exactly as it does in the
        // reference implementation, which simply counts the leading zeros.
        return static_cast<int32_t>(sizeof(int64_t) * 8);
    }
    int32_t bits = 0;
    while (max_value != 0) {
        max_value = static_cast<int64_t>(static_cast<uint64_t>(max_value) >> 1);
        bits += 1;
    }
    // `Packed64::create` needs room for at least one bit.
    return std::max(1, bits);
}

} // namespace

// ---------------------------------------------------------------------------
// PackedLongValuesIterator
// ---------------------------------------------------------------------------

PackedLongValuesIterator::PackedLongValuesIterator(const PackedLongValues &values) : values_(&values) {}

VoidResult PackedLongValuesIterator::ensure_decoded() {
    const size_t block = index_ / values_->page_size();
    if (block == block_index_ && !block_.empty()) {
        return {};
    }
    if (block >= values_->page_count()) {
        block_.clear();
        return err(LuceneError::illegal_state("packed long values iterator is past the last block"));
    }
    auto decoded = values_->decode_block(block, block_);
    if (!decoded.has_value()) {
        return std::unexpected(std::move(decoded.error()));
    }
    block_index_ = block;
    return {};
}

Result<bool> PackedLongValuesIterator::has_next() const { return index_ < static_cast<size_t>(values_->size()); }

Result<int64_t> PackedLongValuesIterator::next() {
    if (index_ >= static_cast<size_t>(values_->size())) {
        return err(LuceneError::illegal_state("packed long values iterator is exhausted"));
    }
    if (auto decoded = ensure_decoded(); !decoded.has_value()) {
        return std::unexpected(std::move(decoded.error()));
    }
    const auto offset = index_ % values_->page_size();
    index_ += 1;
    if (offset >= block_.size()) {
        return err(LuceneError::illegal_state("packed block is shorter than expected"));
    }
    return block_[offset];
}

// ---------------------------------------------------------------------------
// PackedLongValues
// ---------------------------------------------------------------------------

Result<int64_t> PackedLongValues::get(size_t index) const {
    if (index >= size_) {
        return err(LuceneError::illegal_argument("index " + std::to_string(index) +
                                                " is out of bounds for " + std::to_string(size_) + " values"));
    }
    const size_t block = index / page_size_;
    const size_t offset = index % page_size_;
    const auto &page = pages_.at(block);
    debug_check(offset < page.count, "the packed page is shorter than the page size");
    return wrapping_add(page.data.get(offset), page.base);
}

Result<size_t> PackedLongValues::decode_block(size_t block, std::vector<int64_t> &dest) const {
    if (block >= pages_.size()) {
        return err(LuceneError::illegal_argument("block " + std::to_string(block) + " is out of bounds"));
    }
    const auto &page = pages_[block];
    dest.resize(page.count);
    for (size_t i = 0; i < page.count; ++i) {
        dest[i] = wrapping_add(page.data.get(i), page.base);
    }
    return page.count;
}

std::unique_ptr<PackedLongValuesIterator> PackedLongValues::iterator() const {
    return std::make_unique<PackedLongValuesIterator>(*this);
}

int64_t PackedLongValues::ram_bytes_used() const {
    int64_t bytes = 0;
    for (const auto &page : pages_) {
        bytes += static_cast<int64_t>(page.data.ram_bytes_used());
    }
    return bytes;
}

// ---------------------------------------------------------------------------
// PackedLongValues::Builder
// ---------------------------------------------------------------------------

PackedLongValues::Builder::Builder(size_t page_size, int32_t bits_per_value, int32_t initial_page_count, bool delta)
        : page_size_(page_size), bits_per_value_(bits_per_value), delta_(delta) {
    debug_check(page_size_ > 0, "the page size must be positive");
    if (initial_page_count > 0) {
        values_.reserve((static_cast<size_t>(initial_page_count) + 1) * page_size_);
    }
    pending_.reserve(page_size_);
}

VoidResult PackedLongValues::Builder::pack(std::vector<int64_t> &pending, size_t num_values) {
    if (delta_) {
        // Each page keeps its own minimum, like Lucene's
        // `DeltaPackedLongValues`: a page that decreases cannot corrupt the
        // pages around it.
        auto min = pending.front();
        for (const auto value : pending) {
            min = std::min(min, value);
        }
        if (min != 0) {
            // Subtracting a zero minimum is a full pass over the page for
            // nothing; monotonic sequences (doc ids, offsets) often start at 0.
            for (auto &value : pending) {
                value = wrapping_sub(value, min);
            }
        }
        mins_.push_back(min);
    }

    values_.resize(num_values + pending.size());
    for (size_t i = 0; i < pending.size(); ++i) {
        values_[num_values + i] = pending[i];
    }
    page_count_ += 1;
    return {};
}

VoidResult PackedLongValues::Builder::add(int64_t value) {
    if (built_) {
        return err(LuceneError::illegal_state("the packed long values builder has already been built"));
    }
    pending_.push_back(value);
    size_ += 1;
    if (pending_.size() == page_size_) {
        if (auto packed = pack(pending_, num_values_); !packed.has_value()) {
            return packed;
        }
        num_values_ += pending_.size();
        pending_.clear();
    }
    return {};
}

Result<PackedLongValues> PackedLongValues::Builder::build() {
    if (built_) {
        return err(LuceneError::illegal_state("the packed long values builder has already been built"));
    }
    built_ = true;

    if (!pending_.empty()) {
        if (auto packed = pack(pending_, num_values_); !packed.has_value()) {
            return std::unexpected(std::move(packed.error()));
        }
        num_values_ += pending_.size();
        pending_.clear();
    }

    PackedLongValues result;
    result.page_size_ = page_size_;
    result.size_ = size_;
    if (size_ == 0) {
        return result;
    }

    // The staging area layout is: values, delta flag, one min slot per page.
    const size_t values_length = num_values_;
    int32_t max_bits = 0;
    for (size_t i = 0; i < values_length; ++i) {
        auto bits = bits_required(values_[i]);
        if (!bits.has_value()) {
            return std::unexpected(std::move(bits.error()));
        }
        max_bits = std::max(max_bits, bits.value());
    }

    result.pages_.reserve(page_count_);
    for (size_t page = 0; page < page_count_; ++page) {
        const size_t start = page * page_size_;
        const size_t count = std::min(page_size_, values_length - start);
        auto data = Packed64::create(count, max_bits);
        if (!data.has_value()) {
            return std::unexpected(std::move(data.error()));
        }
        for (size_t i = 0; i < count; ++i) {
            if (auto set = data.value().set(i, values_[start + i]); !set.has_value()) {
                return std::unexpected(std::move(set.error()));
            }
        }
        const int64_t base = delta_ ? mins_[page] : 0;
        result.pages_.emplace_back(std::move(data.value()), base, count);
    }
    return result;
}

} // namespace lucene
