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

export module lucene_core:long_bit_set;

import :types;
import std;

import :error;

namespace lucene {

/// A fixed-size bit set addressed by a 64-bit index.
///
/// Port of `LongBitSet` (`util/long_bit_set.rs`).  Unlike
/// [`FixedBitSet`](lucene_core:fixed_bit_set), whose length is a `size_t` number
/// of bits and which is tuned for document ids, this one takes the number of
/// *bits as a `int64_t`* and grows its word array from it, which is what the
/// merge and doc values code wants for per-value bookkeeping.
export class LongBitSet {
public:
    /// A set holding `num_bits` zeroed bits.
    [[nodiscard]] static Result<LongBitSet> create(int64_t num_bits);

    /// `true` when `index` is set; an out-of-range index is an error.
    [[nodiscard]] Result<bool> get(int64_t index) const;
    /// Sets `index`.
    [[nodiscard]] VoidResult set(int64_t index);
    /// Clears `index`.
    [[nodiscard]] VoidResult clear(int64_t index);
    /// Sets `[from, to)`.
    [[nodiscard]] VoidResult set_range(int64_t from, int64_t to);
    /// Clears `[from, to)`.
    [[nodiscard]] VoidResult clear_range(int64_t from, int64_t to);
    /// Flips `index`.
    [[nodiscard]] VoidResult flip(int64_t index);

    /// Clears every bit.
    void clear_all() noexcept;
    /// Sets every bit of the range the set covers.
    [[nodiscard]] VoidResult set_all();

    /// Number of bits set.
    [[nodiscard]] int64_t cardinality() const noexcept;
    /// Number of bits the set can hold.
    [[nodiscard]] int64_t length() const noexcept { return num_bits_; }
    /// `true` when no bit is set.
    [[nodiscard]] bool is_empty() const noexcept { return cardinality() == 0; }

    /// The first bit at or after `index`, or `-1`.
    [[nodiscard]] int64_t next_set_bit(int64_t index) const;
    /// The first bit at or before `index`, or `-1`.
    [[nodiscard]] int64_t prev_set_bit(int64_t index) const;

    /// The bits of `other` that are not bits of this set, in this set.
    [[nodiscard]] VoidResult and_not(const LongBitSet &other);
    /// The bits of `other`, in this set.
    [[nodiscard]] VoidResult or_with(const LongBitSet &other);
    /// Only the bits `other` also holds.
    [[nodiscard]] VoidResult and_with(const LongBitSet &other);
    /// The bits exactly one of the two holds.
    [[nodiscard]] VoidResult xor_with(const LongBitSet &other);

    [[nodiscard]] int64_t ram_bytes_used() const noexcept {
        return static_cast<int64_t>(words_.size() * sizeof(uint64_t));
    }

private:
    LongBitSet(std::vector<uint64_t> words, int64_t num_bits) : words_(std::move(words)), num_bits_(num_bits) {}

    /// Throws the range error the reference implementation reports.
    [[nodiscard]] VoidResult check_index(int64_t index) const;

    std::vector<uint64_t> words_;
    int64_t num_bits_{0};
};

} // namespace lucene
