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

module lucene_core:long_bit_set.impl;

import :long_bit_set;

import :error;
import :types;

import std;

namespace lucene {

namespace {

constexpr int32_t kBitsPerWord = 64;

[[nodiscard]] size_t word_index(int64_t bit) { return static_cast<size_t>(bit >> 6); }
[[nodiscard]] uint64_t bit_mask(int64_t bit) { return uint64_t{1} << (static_cast<int32_t>(bit) & 63); }

[[nodiscard]] int32_t popcount(uint64_t word) { return std::popcount(word); }

} // namespace

Result<LongBitSet> LongBitSet::create(int64_t num_bits) {
    if (num_bits < 0) {
        return err(LuceneError::illegal_argument("num_bits must not be negative, got " + std::to_string(num_bits)));
    }
    // One bit still needs one word to live in.
    const size_t words = num_bits == 0 ? 1 : static_cast<size_t>((num_bits + kBitsPerWord - 1) / kBitsPerWord);
    return LongBitSet(std::vector<uint64_t>(words, 0), num_bits);
}

VoidResult LongBitSet::check_index(int64_t index) const {
    if (index < 0 || index >= num_bits_) {
        return err(LuceneError::array_index_out_of_bounds("index=" + std::to_string(index) + " of a bit set of length " +
                                                    std::to_string(num_bits_)));
    }
    return {};
}

Result<bool> LongBitSet::get(int64_t index) const {
    if (auto checked = check_index(index); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return (words_[word_index(index)] & bit_mask(index)) != 0;
}

VoidResult LongBitSet::set(int64_t index) {
    if (auto checked = check_index(index); !checked.has_value()) {
        return checked;
    }
    words_[word_index(index)] |= bit_mask(index);
    return {};
}

VoidResult LongBitSet::clear(int64_t index) {
    if (auto checked = check_index(index); !checked.has_value()) {
        return checked;
    }
    words_[word_index(index)] &= ~bit_mask(index);
    return {};
}

VoidResult LongBitSet::flip(int64_t index) {
    if (auto checked = check_index(index); !checked.has_value()) {
        return checked;
    }
    words_[word_index(index)] ^= bit_mask(index);
    return {};
}

VoidResult LongBitSet::set_range(int64_t from, int64_t to) {
    if (from < 0 || to > num_bits_ || from > to) {
        return err(LuceneError::illegal_argument("cannot set [" + std::to_string(from) + ", " + std::to_string(to) +
                                                 ") of a bit set of length " + std::to_string(num_bits_)));
    }
    for (int64_t bit = from; bit < to; ++bit) {
        words_[word_index(bit)] |= bit_mask(bit);
    }
    return {};
}

VoidResult LongBitSet::clear_range(int64_t from, int64_t to) {
    if (from < 0 || to > num_bits_ || from > to) {
        return err(LuceneError::illegal_argument("cannot clear [" + std::to_string(from) + ", " +
                                                 std::to_string(to) + ") of a bit set of length " +
                                                 std::to_string(num_bits_)));
    }
    for (int64_t bit = from; bit < to; ++bit) {
        words_[word_index(bit)] &= ~bit_mask(bit);
    }
    return {};
}

void LongBitSet::clear_all() noexcept {
    std::ranges::fill(words_, uint64_t{0});
}

VoidResult LongBitSet::set_all() {
    std::ranges::fill(words_, std::numeric_limits<uint64_t>::max());
    // The bits past the end stay clear, so `cardinality` and `next_set_bit` agree
    // with `length`.
    const int32_t used = static_cast<int32_t>(num_bits_ & 63);
    if (used != 0 && !words_.empty()) {
        words_.back() &= (uint64_t{1} << used) - 1;
    }
    return {};
}

int64_t LongBitSet::cardinality() const noexcept {
    int64_t count = 0;
    for (const uint64_t word : words_) {
        count += popcount(word);
    }
    return count;
}

int64_t LongBitSet::next_set_bit(int64_t index) const {
    if (index < 0) {
        index = 0;
    }
    if (index >= num_bits_) {
        return -1;
    }
    size_t word = word_index(index);
    uint64_t bits = words_[word] & (~uint64_t{0} << (static_cast<int32_t>(index) & 63));
    while (true) {
        if (bits != 0) {
            const int64_t found = static_cast<int64_t>(word) * kBitsPerWord + std::countr_zero(bits);
            return found >= num_bits_ ? -1 : found;
        }
        if (++word >= words_.size()) {
            return -1;
        }
        bits = words_[word];
    }
}

int64_t LongBitSet::prev_set_bit(int64_t index) const {
    if (index >= num_bits_) {
        index = num_bits_ - 1;
    }
    if (index < 0) {
        return -1;
    }
    size_t word = word_index(index);
    const int32_t offset = static_cast<int32_t>(index) & 63;
    uint64_t bits = words_[word] & (offset == 63 ? ~uint64_t{0} : ((uint64_t{1} << (offset + 1)) - 1));
    while (true) {
        if (bits != 0) {
            return static_cast<int64_t>(word) * kBitsPerWord + (63 - std::countl_zero(bits));
        }
        if (word == 0) {
            return -1;
        }
        bits = words_[--word];
    }
}

VoidResult LongBitSet::and_not(const LongBitSet &other) {
    if (other.num_bits_ != num_bits_) {
        return err(LuceneError::illegal_argument("the bit sets have different lengths"));
    }
    for (size_t i = 0; i < words_.size(); ++i) {
        words_[i] &= ~other.words_[i];
    }
    return {};
}

VoidResult LongBitSet::or_with(const LongBitSet &other) {
    if (other.num_bits_ != num_bits_) {
        return err(LuceneError::illegal_argument("the bit sets have different lengths"));
    }
    for (size_t i = 0; i < words_.size(); ++i) {
        words_[i] |= other.words_[i];
    }
    return {};
}

VoidResult LongBitSet::and_with(const LongBitSet &other) {
    if (other.num_bits_ != num_bits_) {
        return err(LuceneError::illegal_argument("the bit sets have different lengths"));
    }
    for (size_t i = 0; i < words_.size(); ++i) {
        words_[i] &= other.words_[i];
    }
    return {};
}

VoidResult LongBitSet::xor_with(const LongBitSet &other) {
    if (other.num_bits_ != num_bits_) {
        return err(LuceneError::illegal_argument("the bit sets have different lengths"));
    }
    for (size_t i = 0; i < words_.size(); ++i) {
        words_[i] ^= other.words_[i];
    }
    return {};
}

} // namespace lucene
