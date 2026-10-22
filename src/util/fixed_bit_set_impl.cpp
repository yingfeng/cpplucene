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

module lucene_core:fixed_bit_set.impl;

import :fixed_bit_set;

import :bits;
import :debug;
import :error;
import :types;

import std;

namespace lucene {

FixedBitSet::FixedBitSet(size_t num_bits) : words_(bits_to_words(num_bits), 0), num_bits_(num_bits) {}

Result<FixedBitSet> FixedBitSet::with_capacity(std::vector<uint64_t> words, size_t num_bits) {
    const size_t needed = bits_to_words(num_bits);
    if (words.size() < needed) {
        return err(LuceneError::illegal_argument("the word array is too short for " + std::to_string(num_bits) +
                                                 " bits: got " + std::to_string(words.size()) + " words, need " +
                                                 std::to_string(needed)));
    }
    words.resize(needed);
    FixedBitSet set;
    set.words_ = std::move(words);
    set.num_bits_ = num_bits;
    set.trim_last_word();
    return set;
}

bool FixedBitSet::get_bit(size_t index) const noexcept {
    debug_check(index < num_bits_, "index out of range");
    return (words_[index >> 6] & (uint64_t{1} << (index & 63))) != 0;
}

Result<bool> FixedBitSet::get(size_t index) const {
    if (index >= num_bits_) {
        return err(LuceneError::array_index_out_of_bounds("index " + std::to_string(index) + " >= " +
                                                         std::to_string(num_bits_)));
    }
    return get_bit(index);
}

bool FixedBitSet::set(size_t index) noexcept {
    debug_check(index < num_bits_, "index out of range");
    const uint64_t mask = uint64_t{1} << (index & 63);
    uint64_t &word = words_[index >> 6];
    const bool was_set = (word & mask) != 0;
    word |= mask;
    return !was_set;
}

bool FixedBitSet::clear(size_t index) noexcept {
    debug_check(index < num_bits_, "index out of range");
    const uint64_t mask = uint64_t{1} << (index & 63);
    uint64_t &word = words_[index >> 6];
    const bool was_set = (word & mask) != 0;
    word &= ~mask;
    return was_set;
}

void FixedBitSet::clear_all() noexcept { std::ranges::fill(words_, uint64_t{0}); }

bool FixedBitSet::flip(size_t index) noexcept {
    debug_check(index < num_bits_, "index out of range");
    uint64_t &word = words_[index >> 6];
    const uint64_t mask = uint64_t{1} << (index & 63);
    const bool was_set = (word & mask) != 0;
    word ^= mask;
    return was_set;
}

size_t FixedBitSet::cardinality() const noexcept {
    size_t count = 0;
    for (const uint64_t word : words_) {
        count += static_cast<size_t>(std::popcount(word));
    }
    return count;
}

int64_t FixedBitSet::next_set_bit(size_t from) const noexcept {
    if (from >= num_bits_) {
        return -1;
    }
    size_t word_index = from >> 6;
    // Mask off the bits before `from` in the first word.
    uint64_t word = words_[word_index] & (~uint64_t{0} << (from & 63));
    while (true) {
        if (word != 0) {
            const size_t bit = static_cast<size_t>(std::countr_zero(word));
            const size_t index = (word_index << 6) + bit;
            return index < num_bits_ ? static_cast<int64_t>(index) : -1;
        }
        ++word_index;
        if (word_index >= words_.size()) {
            return -1;
        }
        word = words_[word_index];
    }
}

bool FixedBitSet::intersects(const FixedBitSet &other) const {
    const size_t common = std::min(words_.size(), other.words_.size());
    for (size_t i = 0; i < common; ++i) {
        if ((words_[i] & other.words_[i]) != 0) {
            return true;
        }
    }
    return false;
}

size_t FixedBitSet::intersection_count(const FixedBitSet &left, const FixedBitSet &right) {
    const size_t common = std::min(left.words_.size(), right.words_.size());
    size_t count = 0;
    for (size_t i = 0; i < common; ++i) {
        count += static_cast<size_t>(std::popcount(left.words_[i] & right.words_[i]));
    }
    return count;
}

void FixedBitSet::or_with(const FixedBitSet &other) {
    if (words_.size() < other.words_.size()) {
        words_.resize(other.words_.size(), 0);
    }
    for (size_t i = 0; i < other.words_.size(); ++i) {
        words_[i] |= other.words_[i];
    }
    trim_last_word();
}

void FixedBitSet::and_with(const FixedBitSet &other) {
    const size_t common = std::min(words_.size(), other.words_.size());
    for (size_t i = 0; i < common; ++i) {
        words_[i] &= other.words_[i];
    }
    // Bits past the end of `other` cannot be set in the intersection.
    std::fill(words_.begin() + static_cast<std::ptrdiff_t>(common), words_.end(), 0);
}

void FixedBitSet::and_not(const FixedBitSet &other) {
    const size_t common = std::min(words_.size(), other.words_.size());
    for (size_t i = 0; i < common; ++i) {
        words_[i] &= ~other.words_[i];
    }
    trim_last_word();
}

void FixedBitSet::trim_last_word() noexcept {
    if (num_bits_ == 0 || words_.empty()) {
        return;
    }
    const size_t used = num_bits_ & 63;
    if (used != 0) {
        words_.back() &= (uint64_t{1} << used) - 1;
    }
}

std::string FixedBitSet::to_string() const {
    return "FixedBitSet(length=" + std::to_string(num_bits_) + ", cardinality=" + std::to_string(cardinality()) + ")";
}

} // namespace lucene
