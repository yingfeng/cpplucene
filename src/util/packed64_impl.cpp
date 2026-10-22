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

module lucene_core:packed64.impl;

import :packed64;
import :packed_ints;
import :types;
import :error;
import :debug;

import std;

namespace lucene {

Packed64::Packed64(size_t value_count, int32_t bits_per_value, std::vector<uint64_t> words)
    : value_count_(value_count), bits_per_value_(bits_per_value), words_(std::move(words)) {}

Result<Packed64> Packed64::create(size_t value_count, int32_t bits_per_value) {
    if (bits_per_value < 1 || bits_per_value > 64) {
        return err(LuceneError::illegal_argument("bitsPerValue must be in [1, 64], got " +
                                                 std::to_string(bits_per_value)));
    }
    // One extra word so that a value straddling a word boundary can be read
    // without a branch, like Lucene's `Packed64`.
    const size_t bits = value_count * static_cast<size_t>(bits_per_value);
    const size_t word_count = bits / 64 + 2;
    return Packed64(value_count, bits_per_value, std::vector<uint64_t>(word_count, 0));
}

uint64_t Packed64::mask() const noexcept {
    return bits_per_value_ >= 64 ? std::numeric_limits<uint64_t>::max() : ((uint64_t{1} << bits_per_value_) - 1);
}

int64_t Packed64::get(size_t index) const noexcept {
    debug_check(index < value_count_, "index out of bounds");
    const size_t bit_pos = index * static_cast<size_t>(bits_per_value_);
    const size_t word_index = bit_pos >> 6;
    const size_t offset = bit_pos & 63;
    const size_t remaining = 64 - offset;
    uint64_t value = words_[word_index] >> offset;
    if (remaining < static_cast<size_t>(bits_per_value_)) {
        value |= words_[word_index + 1] << remaining;
    }
    return static_cast<int64_t>(value & mask());
}

VoidResult Packed64::set(size_t index, int64_t value) {
    if (index >= value_count_) {
        return err(LuceneError::array_index_out_of_bounds("index " + std::to_string(index) + " >= " +
                                                         std::to_string(value_count_)));
    }
    const auto unsigned_value = static_cast<uint64_t>(value);
    if (bits_per_value_ < 64 && (value < 0 || (unsigned_value & mask()) != unsigned_value)) {
        return err(LuceneError::illegal_argument("value " + std::to_string(value) + " does not fit " +
                                                 std::to_string(bits_per_value_) + " bits"));
    }

    const size_t bit_pos = index * static_cast<size_t>(bits_per_value_);
    const size_t word_index = bit_pos >> 6;
    const size_t offset = bit_pos & 63;
    const uint64_t value_mask = mask();

    // Low part: bits `[offset, min(64, offset + bits_per_value))`.
    const uint64_t low_mask = value_mask << offset;
    words_[word_index] = (words_[word_index] & ~low_mask) | ((unsigned_value << offset) & low_mask);

    // High part: the remaining bits go into the next word.
    const size_t remaining = 64 - offset;
    if (remaining < static_cast<size_t>(bits_per_value_)) {
        const uint64_t high_mask = value_mask >> remaining;
        words_[word_index + 1] =
            (words_[word_index + 1] & ~high_mask) | ((unsigned_value >> remaining) & high_mask);
    }
    return {};
}

VoidResult Packed64::fill(size_t from_index, size_t to_index, int64_t value) {
    if (from_index > to_index || to_index > value_count_) {
        return err(LuceneError::array_index_out_of_bounds("range [" + std::to_string(from_index) + ", " +
                                                         std::to_string(to_index) + ") is invalid"));
    }
    for (size_t i = from_index; i < to_index; ++i) {
        if (auto result = set(i, value); !result.has_value()) {
            return result;
        }
    }
    return {};
}

VoidResult Packed64::copy_from(const Packed64 &source, size_t src_pos, size_t dest_pos, size_t len) {
    if (src_pos + len > source.size() || dest_pos + len > value_count_) {
        return err(LuceneError::array_index_out_of_bounds("copy range is out of bounds"));
    }
    for (size_t i = 0; i < len; ++i) {
        if (auto result = set(dest_pos + i, source.get(src_pos + i)); !result.has_value()) {
            return result;
        }
    }
    return {};
}

void Packed64::clear() noexcept {
    std::fill(words_.begin(), words_.end(), 0);
}

} // namespace lucene
