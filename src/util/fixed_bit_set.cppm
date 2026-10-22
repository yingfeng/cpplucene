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

export module lucene_core:fixed_bit_set;

import :types;
import :error;

import std;

import :bits;

namespace lucene {

/// A mutable, fixed length bit set stored as 64 bit words.
///
/// Port of `FixedBitSet` (`util/fixed_bit_set.rs`), reduced to the operations the
/// codecs need: construction from words (the on-disk form), single bit
/// get/set/clear/flip, population count, and the set operations deletions use.
/// The reference implementation's separate read-only `FixedBit` wrapper is not needed here because
/// [`FixedBitSet`] already implements [`Bits`].
export class FixedBitSet final : public Bits {
public:
    /// Empty set of `num_bits` bits.
    explicit FixedBitSet(size_t num_bits = 0);

    /// Adopts `words`, which must have room for `num_bits` bits.
    [[nodiscard]] static Result<FixedBitSet> with_capacity(std::vector<uint64_t> words, size_t num_bits);

    /// Number of 64 bit words needed for `num_bits` bits.
    [[nodiscard]] static constexpr size_t bits_to_words(size_t num_bits) noexcept {
        return (num_bits + 63) / 64;
    }

    [[nodiscard]] Result<bool> get(size_t index) const override;
    [[nodiscard]] size_t length() const override { return num_bits_; }

    /// Value of the bit at `index`; `index` must be in range.
    [[nodiscard]] bool get_bit(size_t index) const noexcept;

    /// Sets the bit at `index` and returns whether it was clear before.
    bool set(size_t index) noexcept;
    /// Clears the bit at `index` and returns whether it was set before.
    bool clear(size_t index) noexcept;
    /// Grows the set so that `index` fits, keeping the existing bits.
    [[nodiscard]] VoidResult ensure_capacity(size_t index);
    /// Sets every bit in `[from, to)`; `to` must not be past the length.
    [[nodiscard]] VoidResult set_with_range(size_t from, size_t to);
    /// Clears every bit, keeping the length; the `clear` of the reference implementation's `BitSet`.
    void clear_all() noexcept;
    /// Flips the bit at `index` and returns whether it was set before.
    bool flip(size_t index) noexcept;

    /// Number of set bits.
    [[nodiscard]] size_t cardinality() const noexcept;

    /// First set bit at or after `from`, or `-1`.
    [[nodiscard]] int64_t next_set_bit(size_t from) const noexcept;

    [[nodiscard]] const std::vector<uint64_t> &words() const noexcept { return words_; }

    /// `true` when at least one bit is set in both sets.
    [[nodiscard]] bool intersects(const FixedBitSet &other) const;

    /// In-place union, intersection and difference; `other` may be longer.
    void or_with(const FixedBitSet &other);
    void and_with(const FixedBitSet &other);
    void and_not(const FixedBitSet &other);

    /// Number of set bits in the intersection, without materialising it.
    [[nodiscard]] static size_t intersection_count(const FixedBitSet &left, const FixedBitSet &right);

    [[nodiscard]] std::string to_string() const override;

private:
    /// Drops bits at or past `num_bits_` so the last word stays clean.
    void trim_last_word() noexcept;

    std::vector<uint64_t> words_;
    size_t num_bits_{0};
};

} // namespace lucene
