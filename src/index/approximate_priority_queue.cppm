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

export module lucene_core:approximate_priority_queue;

import :types;
import std;

namespace lucene {

/// A priority queue that polls entries roughly by decreasing weight, without
/// guaranteeing the exact order.
///
/// Port of `ApproximatePriorityQueue` (`core/index/approximate_priority_queue.rs`).
///
/// The queue keeps the first [`SPARSE_SLOTS`] slots apart, one per class of
/// leading zeros of the weight, so a heavier entry lands in a lower slot and is
/// polled first.  Entries whose weight has fewer significant bits than
/// [`SPARSE_SLOTS`] (weight `0` in practice) do not fit those classes and go to a
/// dense tail that is drained from its end, which keeps the last used entry hot.
///
/// An entry has to expose its identity, because the queue also removes single
/// entries by id; the reference implementation asks for the same through its
/// `IdentityId` trait.
export template <typename T>
class ApproximatePriorityQueue {
public:
    /// How many weights are told apart by their class of leading zeros; the
    /// header of a 64 bit weight holds one class per possible count.
    static constexpr size_t SPARSE_SLOTS = 64;

    ApproximatePriorityQueue() : slots_(SPARSE_SLOTS) {}

    /// Adds `entry` with the given `weight`; a heavier weight is polled earlier.
    void add(T entry, int64_t weight) {
        // The expected slot is the number of leading zeros of the weight: the
        // heavier the weight, the lower the slot.
        const auto expected_slot = static_cast<size_t>(std::countl_zero(static_cast<uint64_t>(weight)));
        // When that slot is taken, the first free one after it is used: shifting
        // the free-slot bitset right by the expected slot and counting the
        // trailing zeros is the same as looping until a free slot turns up.
        const uint64_t free_slots = ~used_slots_;
        const auto offset = static_cast<size_t>(std::countr_zero(shift_right(free_slots, expected_slot)));
        const size_t destination = expected_slot + offset;
        if (destination < SPARSE_SLOTS) {
            used_slots_ |= uint64_t{1} << destination;
            slots_[destination] = std::move(entry);
            return;
        }
        slots_.emplace_back(std::move(entry));
    }

    /// Returns the first entry `predicate` accepts, roughly the heaviest one, or
    /// `nullopt` when no entry is left that it accepts.
    template <typename Predicate>
    [[nodiscard]] std::optional<T> poll(Predicate predicate) {
        // The sparse slots come first, in increasing order, so the heaviest
        // entry that matches wins.
        size_t next_slot = 0;
        while (next_slot < SPARSE_SLOTS) {
            const size_t next_used_slot =
                    next_slot + static_cast<size_t>(std::countr_zero(shift_right(used_slots_, next_slot)));
            if (next_used_slot >= SPARSE_SLOTS) {
                break;
            }
            if (slots_[next_used_slot].has_value() && predicate(slots_[next_used_slot].value())) {
                used_slots_ &= ~(uint64_t{1} << next_used_slot);
                std::optional<T> taken = std::move(slots_[next_used_slot]);
                slots_[next_used_slot].reset();
                return taken;
            }
            next_slot = next_used_slot + 1;
        }
        // Then the dense tail, from its end: the entry that was added last is
        // polled first, so the queue keeps handing out the same one.
        for (size_t i = slots_.size(); i > SPARSE_SLOTS; --i) {
            const size_t index = i - 1;
            if (slots_[index].has_value() && predicate(slots_[index].value())) {
                std::optional<T> taken = std::move(slots_[index]);
                slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(index));
                return taken;
            }
        }
        return std::nullopt;
    }

    /// Whether an entry with this id is queued.
    [[nodiscard]] bool contains(std::string_view id) const {
        for (const std::optional<T> &slot : slots_) {
            if (slot.has_value() && std::string_view(slot->id()) == id) {
                return true;
            }
        }
        return false;
    }

    /// Removes the entry with this id, or answers `nullopt` when it is not queued.
    [[nodiscard]] std::optional<T> remove(std::string_view id) {
        for (size_t index = 0; index < slots_.size(); ++index) {
            if (!slots_[index].has_value() || std::string_view(slots_[index]->id()) != id) {
                continue;
            }
            std::optional<T> removed = std::move(slots_[index]);
            if (index < SPARSE_SLOTS) {
                used_slots_ &= ~(uint64_t{1} << index);
                slots_[index].reset();
            } else {
                slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(index));
            }
            return removed;
        }
        return std::nullopt;
    }

    /// Whether the queue holds no entry.
    [[nodiscard]] bool is_empty() const noexcept {
        return used_slots_ == 0 && slots_.size() == SPARSE_SLOTS;
    }

    /// How many entries are queued.
    [[nodiscard]] size_t size() const noexcept {
        size_t count = 0;
        for (const std::optional<T> &slot : slots_) {
            if (slot.has_value()) {
                ++count;
            }
        }
        return count;
    }

private:
    /// `value >> shift`, answering `0` when the shift is past the width, which is
    /// what the reference implementation's wrapping shift does in the same case.
    [[nodiscard]] static uint64_t shift_right(uint64_t value, size_t shift) noexcept {
        return shift >= 64 ? 0 : value >> shift;
    }

    /// Indexes below [`SPARSE_SLOTS`] hold at most one entry each; indexes from
    /// there on hold the entries that did not fit a class.
    std::vector<std::optional<T>> slots_;
    /// A bit per sparse slot, set while that slot is taken.
    uint64_t used_slots_{0};
};

} // namespace lucene
