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

export module lucene_core:int_block_pool;

import :types;
import std;

import :error;

namespace lucene {

/// Log2 of the number of `int32_t` slots in one block.
export inline constexpr int32_t INT_BLOCK_SHIFT = 13;
/// Number of `int32_t` slots in one block (8192).
export inline constexpr int32_t INT_BLOCK_SIZE = 1 << INT_BLOCK_SHIFT;
/// Mask of the slot index inside a block.
export inline constexpr int32_t INT_BLOCK_MASK = INT_BLOCK_SIZE - 1;

/// Supplies and recycles the fixed-size blocks of an [`IntBlockPool`].
///
/// Port of the `AllocatorI32` trait (`util/int_block_pool.rs`).
export class IntBlockAllocator {
public:
    virtual ~IntBlockAllocator() = default;

    /// A fresh, zeroed block of `INT_BLOCK_SIZE` slots.
    [[nodiscard]] virtual std::vector<int32_t> get_int_block() = 0;

    /// Called with the blocks `[start, end)` that the pool no longer uses.
    virtual void recycle_int_blocks(std::span<const std::vector<int32_t>> blocks, size_t start, size_t end) {
        (void)blocks;
        (void)start;
        (void)end;
    }

    [[nodiscard]] virtual size_t block_size() const { return static_cast<size_t>(INT_BLOCK_SIZE); }
};

/// The allocator that always allocates.
export class DirectIntBlockAllocator final : public IntBlockAllocator {
public:
    [[nodiscard]] std::vector<int32_t> get_int_block() override;
};

/// The integer twin of [`ByteBlockPool`](:byte_block_pool).
///
/// Port of `util/int_block_pool.rs`.  The indexing chain writes the
/// per-document `int` postings (doc codes, term frequencies, positions,
/// offsets) into blocks of this pool, addressed by a flat `int_offset`
/// plus `int_upto` exactly like the byte pool addresses its blocks.
///
/// **Lifetime:** the pool is referenced from [`FreqProxFields`] while a segment
/// is flushed, so callers hold it behind a `std::shared_ptr`.
export class IntBlockPool {
public:
    IntBlockPool();
    explicit IntBlockPool(std::unique_ptr<IntBlockAllocator> allocator);

    IntBlockPool(const IntBlockPool &) = delete;
    IntBlockPool &operator=(const IntBlockPool &) = delete;
    IntBlockPool(IntBlockPool &&) noexcept = default;
    IntBlockPool &operator=(IntBlockPool &&) noexcept = default;
    ~IntBlockPool() = default;

    /// Returns the pool to its initial state.
    ///
    /// `zero_fill_buffers` wipes the blocks that are kept; `reuse_first` keeps
    /// the head block and positions the pool at its beginning (so no
    /// [`next_buffer`] is needed), which is what a slice pool relying on
    /// zeroed blocks expects.
    void reset(bool zero_fill_buffers, bool reuse_first);

    /// Appends a block and makes it the head.
    [[nodiscard]] VoidResult next_buffer();

    // ---- state, mirroring the reference implementation's crate-visible fields ----

    [[nodiscard]] const std::vector<std::vector<int32_t>> &buffers() const noexcept { return buffers_; }
    [[nodiscard]] std::optional<size_t> buffer_upto() const noexcept { return buffer_upto_; }
    /// Slot index inside the head block.
    [[nodiscard]] int32_t int_upto() const noexcept { return int_upto_; }
    void set_int_upto(int32_t value) noexcept { int_upto_ = value; }
    /// Flat slot offset of the head block.
    [[nodiscard]] int32_t int_offset() const noexcept { return int_offset_; }
    void set_int_offset(int32_t value) noexcept { int_offset_ = value; }

    [[nodiscard]] std::vector<int32_t> &buffer_mut(size_t index) { return buffers_.at(index); }
    [[nodiscard]] std::span<const int32_t> buffer(size_t index) const { return buffers_.at(index); }

private:
    /// Keeps the default allocator alive when none was supplied.
    std::unique_ptr<IntBlockAllocator> owned_allocator_;
    IntBlockAllocator *allocator_{nullptr};
    std::vector<std::vector<int32_t>> buffers_;
    std::optional<size_t> buffer_upto_;
    int32_t int_upto_{INT_BLOCK_SIZE};
    int32_t int_offset_{-INT_BLOCK_SIZE};
};

} // namespace lucene
