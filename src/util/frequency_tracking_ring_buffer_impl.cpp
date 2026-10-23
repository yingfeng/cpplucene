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

module lucene_core:frequency_tracking_ring_buffer.impl;

import :frequency_tracking_ring_buffer;

import std;

import :debug;
import :error;
import :types;

namespace lucene {

IntBag::IntBag(size_t max_size) {
    size_t capacity = std::max<size_t>(2, max_size * 3 / 2);
    // Round up to the next power of two, so that the mask works.
    size_t rounded = 1;
    while (rounded < capacity) {
        rounded <<= 1;
    }
    debug_check(rounded > max_size, "the bag must be larger than the number of tracked keys");
    keys_.assign(rounded, 0);
    freqs_.assign(rounded, 0);
    mask_ = rounded - 1;
}

int32_t IntBag::frequency(int32_t key) const {
    size_t slot = static_cast<size_t>(key) & mask_;
    for (;;) {
        if (keys_[slot] == key) {
            debug_check(freqs_[slot] >= 0, "frequencies are never negative");
            return freqs_[slot];
        }
        if (freqs_[slot] == 0) {
            return 0;
        }
        slot = (slot + 1) & mask_;
    }
}

int32_t IntBag::add(int32_t key) {
    size_t slot = static_cast<size_t>(key) & mask_;
    for (;;) {
        if (freqs_[slot] == 0) {
            keys_[slot] = key;
            freqs_[slot] = 1;
            return 1;
        }
        if (keys_[slot] == key) {
            ++freqs_[slot];
            return freqs_[slot];
        }
        slot = (slot + 1) & mask_;
    }
}

bool IntBag::remove(int32_t key) {
    size_t slot = static_cast<size_t>(key) & mask_;
    for (;;) {
        if (freqs_[slot] == 0) {
            // No such key in the bag; the chain ends here.
            return false;
        }
        if (keys_[slot] == key) {
            --freqs_[slot];
            if (freqs_[slot] == 0) {
                relocate_adjacent_keys(slot);
            }
            return true;
        }
        slot = (slot + 1) & mask_;
    }
}

void IntBag::relocate_adjacent_keys(size_t free_slot) {
    size_t slot = (free_slot + 1) & mask_;
    for (;;) {
        const int32_t freq = freqs_[slot];
        if (freq == 0) {
            // The end of the collision chain.
            break;
        }
        const int32_t key = keys_[slot];
        const size_t expected_slot = static_cast<size_t>(key) & mask_;
        if (between(expected_slot, slot, free_slot)) {
            keys_[free_slot] = key;
            freqs_[free_slot] = freq;
            freqs_[slot] = 0;
            free_slot = slot;
        }
        slot = (slot + 1) & mask_;
    }
}

bool IntBag::between(size_t chain_start, size_t chain_end, size_t slot) {
    if (chain_start <= chain_end) {
        return chain_start <= slot && slot <= chain_end;
    }
    return slot >= chain_start || slot <= chain_end;
}

FrequencyTrackingRingBuffer::FrequencyTrackingRingBuffer(size_t max_size, int32_t sentinel)
        : buffer_(max_size, sentinel), frequencies_(max_size) {
    for (size_t index = 0; index < max_size; ++index) {
        frequencies_.add(sentinel);
    }
}

Result<FrequencyTrackingRingBuffer> FrequencyTrackingRingBuffer::create(size_t max_size, int32_t sentinel) {
    if (max_size < 2) {
        return err(LuceneError::illegal_argument("max_size must be at least 2"));
    }
    return FrequencyTrackingRingBuffer(max_size, sentinel);
}

void FrequencyTrackingRingBuffer::add(int32_t value) {
    const int32_t removed = buffer_[position_];
    const bool removed_from_bag = frequencies_.remove(removed);
    debug_check(removed_from_bag, "the removed value must be in the bag");

    buffer_[position_] = value;
    frequencies_.add(value);

    ++position_;
    if (position_ == buffer_.size()) {
        position_ = 0;
    }
}

} // namespace lucene
