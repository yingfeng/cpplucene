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

export module lucene_core:frequency_tracking_ring_buffer;

import :types;
import std;

import :error;

namespace lucene {

/// An open-addressed hash table from a 32-bit key to the number of times it is
/// present.
///
/// Port of `IntBag` (`core/util/frequency_tracking_ring_buffer.rs`).  Removal
/// relocates the following keys of the collision chain, so that lookups never
/// run through an empty slot.
export class IntBag {
public:
    /// A bag whose capacity is the next power of two above `max_size * 3 / 2`,
    /// so that it never fills up as long as no more than `max_size` distinct keys
    /// are present.
    explicit IntBag(size_t max_size);

    /// How often `key` is in the bag.
    [[nodiscard]] int32_t frequency(int32_t key) const;

    /// Adds one occurrence of `key` and answers its new frequency.
    int32_t add(int32_t key);

    /// Removes one occurrence of `key`; answers whether it was present.
    bool remove(int32_t key);

    /// The number of slots, for diagnostics.
    [[nodiscard]] size_t capacity() const noexcept { return keys_.size(); }

private:
    /// Moves the keys of the collision chain that follows `free_slot` one slot
    /// back when they can reach their own slot from there.
    void relocate_adjacent_keys(size_t free_slot);

    /// Whether `slot` lies between `chain_start` and `chain_end` on the ring.
    [[nodiscard]] static bool between(size_t chain_start, size_t chain_end, size_t slot);

    std::vector<int32_t> keys_;
    std::vector<int32_t> freqs_;
    size_t mask_;
};

/// A ring buffer that tracks the frequency of the integers it contains, which is
/// typically useful to track the hash codes of popular recently used items.
///
/// Port of `FrequencyTrackingRingBuffer`
/// (`core/util/frequency_tracking_ring_buffer.rs`).  The buffer needs about 22
/// bytes per entry.
export class FrequencyTrackingRingBuffer {
public:
    /// A buffer of at most `max_size` items, initially full of `sentinel`.
    [[nodiscard]] static Result<FrequencyTrackingRingBuffer> create(size_t max_size, int32_t sentinel);

    /// Adds `value`, dropping the oldest entry when the buffer is full.
    void add(int32_t value);

    /// How often `key` is in the buffer.
    [[nodiscard]] int32_t frequency(int32_t key) const { return frequencies_.frequency(key); }

    /// The number of tracked items.
    [[nodiscard]] size_t max_size() const noexcept { return buffer_.size(); }

private:
    FrequencyTrackingRingBuffer(size_t max_size, int32_t sentinel);

    std::vector<int32_t> buffer_;
    size_t position_{0};
    IntBag frequencies_;
};

} // namespace lucene
