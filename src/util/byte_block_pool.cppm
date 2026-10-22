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

export module lucene_core:byte_block_pool;

import :types;
import :error;

import std;

import :bytes_ref;

namespace lucene {

/// Log2 of the size of one pool block.
export inline constexpr int32_t BYTE_BLOCK_SHIFT = 15;
/// Size of one pool block (32kB).
export inline constexpr int32_t BYTE_BLOCK_SIZE = 1 << BYTE_BLOCK_SHIFT;
/// Mask of the offset inside a block.
export inline constexpr int32_t BYTE_BLOCK_MASK = BYTE_BLOCK_SIZE - 1;
/// `~BYTE_BLOCK_MASK`.
export inline constexpr int64_t BYTE_BLOCK_NOT_MASK = ~static_cast<int64_t>(BYTE_BLOCK_MASK);

/// Supplies and recycles the fixed-size blocks of a [`ByteBlockPool`].
///
/// Port of the `AllocatorByte` trait (`util/allocator_byte.rs`).  Recycling is
/// optional: the default allocator simply allocates.
export class ByteBlockAllocator {
public:
    virtual ~ByteBlockAllocator() = default;

    /// A fresh, zeroed block of exactly `BYTE_BLOCK_SIZE` bytes.
    [[nodiscard]] virtual std::vector<uint8_t> get_byte_block() = 0;

    /// Called with the blocks that are no longer referenced by the pool.
    virtual void recycle_byte_blocks(std::span<std::vector<uint8_t>> blocks) { (void)blocks; }
};

/// The allocator that always allocates.
export class DirectByteBlockAllocator final : public ByteBlockAllocator {
public:
    [[nodiscard]] std::vector<uint8_t> get_byte_block() override;
};

/// A list of fixed-size byte blocks, addressed by a flat 64-bit offset.
///
/// Port of `util/byte_block_pool.rs`.  Writes and reads transparently cross
/// block boundaries, which is what the FST builders rely on.
///
/// **Deviation:** the reference implementation returns a borrowed `BytesRef<Cow<Vec<u8>>>` from
/// [`set_bytes_ref`] so an in-block value needs no copy; here the equivalent
/// [`bytes_ref`] always returns an owned `BytesRef`, because C++ `std::span`
/// cannot express "borrowed from a pool that may reallocate".
export class ByteBlockPool {
public:
    ByteBlockPool();
    explicit ByteBlockPool(ByteBlockAllocator &allocator);

    /// Resets the pool; optionally zero-fills the blocks and keeps the first one.
    void reset(bool zero_fill_buffers, bool reuse_first);

    /// Advances to a fresh block and returns its index.
    [[nodiscard]] Result<size_t> next_buffer();

    [[nodiscard]] VoidResult append(std::span<const uint8_t> bytes);
    [[nodiscard]] VoidResult append_range(std::span<const uint8_t> bytes, size_t offset, size_t length);
    [[nodiscard]] VoidResult append(const BytesRef &bytes);
    /// Copies `length` bytes of `source` starting at `src_offset` to the end.
    [[nodiscard]] VoidResult append_from(const ByteBlockPool &source, int64_t src_offset, size_t length);

    /// Copies `length` bytes at `offset` into `dest[dest_offset..]`.
    [[nodiscard]] VoidResult read_bytes(int64_t offset, std::span<uint8_t> dest, size_t dest_offset,
                                        size_t length) const;
    [[nodiscard]] uint8_t read_byte(size_t offset) const;
    /// An owned copy of `length` bytes at `offset`.
    [[nodiscard]] Result<BytesRef> bytes_ref(int64_t offset, size_t length) const;

    // ---- raw cursors, used by the indexing slice pool ----
    //
    // The reference implementation exposes these as crate-visible fields (`byte_upto`, `byte_offset`,
    // `get_buffer_mut`) because the slice pool writes terms and postings
    // directly into the head block.  They are accessors here so the state stays
    // encapsulated, but they carry the same meaning.

    /// Write cursor inside the head block.
    [[nodiscard]] int32_t byte_upto() const noexcept { return byte_upto_; }
    void set_byte_upto(int32_t value) noexcept { byte_upto_ = value; }
    /// Flat offset of the head block.
    [[nodiscard]] int32_t byte_offset() const noexcept { return byte_offset_; }
    /// Mutable access to a whole block.
    [[nodiscard]] std::vector<uint8_t> &mutable_buffer(size_t index) { return buffers_.at(index); }

    /// The current write position: `byte_offset + byte_upto`.
    [[nodiscard]] int64_t get_position() const noexcept {
        return static_cast<int64_t>(byte_offset_) + byte_upto_;
    }
    /// Number of bytes written so far, across all blocks.
    [[nodiscard]] int64_t get_bytes_used() const noexcept;
    /// Index of the block currently being written, if any.
    [[nodiscard]] std::optional<size_t> current_buffer() const noexcept { return buffer_upto_; }
    [[nodiscard]] size_t buffer_count() const noexcept { return buffers_.size(); }
    [[nodiscard]] const std::vector<uint8_t> &buffer(size_t index) const { return buffers_.at(index); }

private:
    /// Keeps the default allocator alive when none was supplied.
    std::unique_ptr<ByteBlockAllocator> owned_allocator_;
    ByteBlockAllocator *allocator_{nullptr};
    std::vector<std::vector<uint8_t>> buffers_;
    std::optional<size_t> buffer_upto_;
    int32_t byte_offset_{-BYTE_BLOCK_SIZE};
    int32_t byte_upto_{BYTE_BLOCK_SIZE};
};

} // namespace lucene
