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

export module lucene_core:rolling_buffer;

import :types;

import std;

namespace lucene {

/// A growable array of reusable slots addressed by absolute position.
///
/// Port of `RollingBuffer` (`util/rolling_buffer.rs`, which follows the Java
/// `RollingBuffer`): position `0` is the first position handed out after
/// [`reset`](RollingBuffer::reset), positions increase monotonically, and
/// [`free_before`](RollingBuffer::free_before) retires the slots of the positions
/// below a caller-chosen bound so they can be recycled for positions further
/// ahead.  `T` must be default constructible and provide `reset()`.
///
/// The reference implementation grows a circular array in place; this port keeps
/// the live slots in a deque plus a free list, which is the same state machine
/// (positions, `max_pos`, `buffer_size`, recycling) with less index arithmetic.
export template <class T>
class RollingBuffer {
public:
    RollingBuffer() = default;

    /// Returns the slot of `pos`, creating (or recycling) it when necessary.
    ///
    /// `pos` may be arbitrarily far in the future, but never before the last
    /// [`free_before`](RollingBuffer::free_before).
    T &get(int32_t pos) {
        // Precondition: `pos >= base_`, i.e. not below the last `free_before`.  The
        // reference implementation asserts this; asking for a retired position is a
        // caller bug (the slot has been recycled), so the clamp below only keeps the
        // read inside the buffer instead of faulting.
        pos = std::max(pos, base_);
        while (static_cast<int32_t>(slots_.size()) + base_ <= pos) {
            if (!free_.empty()) {
                std::unique_ptr<T> slot = std::move(free_.back());
                free_.pop_back();
                slot->reset();
                slots_.push_back(std::move(slot));
            } else {
                slots_.push_back(std::make_unique<T>());
            }
        }
        return *slots_[static_cast<size_t>(pos - base_)];
    }

    /// The highest position handed out so far, or `-1` right after construction/reset.
    [[nodiscard]] int32_t max_pos() const {
        return base_ + static_cast<int32_t>(slots_.size()) - 1;
    }

    /// How many positions are still alive.
    [[nodiscard]] size_t buffer_size() const noexcept { return slots_.size(); }

    /// Drops every position below `pos`, recycling its slot.
    void free_before(int32_t pos) {
        while (base_ < pos && !slots_.empty()) {
            std::unique_ptr<T> slot = std::move(slots_.front());
            slots_.pop_front();
            slot->reset();
            free_.push_back(std::move(slot));
            ++base_;
        }
    }

    /// Retires every position, so the next [`get`](RollingBuffer::get) starts at `0` again.
    void reset() {
        while (!slots_.empty()) {
            std::unique_ptr<T> slot = std::move(slots_.front());
            slots_.pop_front();
            slot->reset();
            free_.push_back(std::move(slot));
        }
        base_ = 0;
    }

private:
    std::deque<std::unique_ptr<T>> slots_{};
    std::vector<std::unique_ptr<T>> free_{};
    int32_t base_{0};
};

} // namespace lucene
