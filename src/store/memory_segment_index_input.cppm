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

export module lucene_core:memory_segment_index_input;

import :types;
import :error;

import std;

import :data_input;
import :index_input;
import :random_access_input;

namespace lucene {

/// Default size of one mapping chunk; large files are mapped in several
/// chunks so that a single mapping never needs more than this.
///
/// The reference implementation uses 16 GiB on 64-bit targets; 1 GiB keeps the
/// number of mappings modest for the index sizes this port targets while still
/// covering every realistic segment in a single chunk.
export inline constexpr size_t DEFAULT_MAPPING_CHUNK_SIZE = size_t{1} << 30;

/// A read-only memory mapping of a whole file, split into fixed size chunks.
///
/// Port of the file mapping used by `memory_segment_index_input.rs` /
/// `mmap_directory.rs`.  Instances are immutable after [`map`] succeeds, so
/// every [`IndexInput`] slice and clone can share one instance.
export class MemoryMapping {
public:
    MemoryMapping() = default;
    MemoryMapping(const MemoryMapping &) = delete;
    MemoryMapping &operator=(const MemoryMapping &) = delete;
    ~MemoryMapping();

    /// Maps `[0, size)` of `fd`.  On failure the object stays empty and the
    /// error describes why (callers may fall back to a non-mapped input).
    [[nodiscard]] VoidResult map(int fd, size_t size, size_t chunk_size);

    /// Total mapped length in bytes.
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] size_t chunk_size() const noexcept { return chunk_size_; }
    [[nodiscard]] size_t chunk_count() const noexcept { return segments_.size(); }
    [[nodiscard]] bool empty() const noexcept { return segments_.empty(); }

    /// Copies `dst.size()` bytes at `pos`; the caller validates the range.
    void read(size_t pos, std::span<uint8_t> dst) const noexcept;

    /// A pointer to `len` bytes at `pos`, or `nullptr` when the range straddles
    /// two mapping chunks or lies outside the mapping.
    ///
    /// This is the fast path of the fixed-width readers: a shifted chunk lookup
    /// and one load instead of a `memcpy` call per value.
    [[nodiscard]] const uint8_t *ptr_at(size_t pos, size_t len) const noexcept;

    /// `[pos, pos + len)` as one contiguous block, or nothing when it straddles
    /// two mapping chunks.
    [[nodiscard]] std::optional<std::span<const uint8_t>> contiguous_span(size_t pos, size_t len) const noexcept;

    /// Advises the kernel that `[pos, pos + len)` will be read soon.
    void prefetch(size_t pos, size_t len) const noexcept;

private:
    std::vector<uint8_t *> segments_;
    std::vector<size_t> segment_sizes_;
    /// Always a power of two, so chunk lookups are shifts (see [`map`]).
    size_t chunk_size_{0};
    size_t chunk_shift_{0};
    size_t size_{0};
};

/// [`IndexInput`] and [`RandomAccessInput`] over a [`MemoryMapping`] region.
///
/// Port of `MemorySegmentIndexInput`.  Every read is a `memcpy` out of the
/// mapping, so no system call and no extra buffering is involved.
export class MemorySegmentIndexInput final : public IndexInput, public RandomAccessInput {
public:
    /// `offset + length` must be within `mapping->size()`.
    MemorySegmentIndexInput(std::shared_ptr<MemoryMapping> mapping,
                            size_t offset,
                            size_t length,
                            std::string description);

    using DataInput::read_bytes;

    [[nodiscard]] const std::shared_ptr<MemoryMapping> &mapping() const noexcept { return mapping_; }

    // ---- IndexInput ----

    [[nodiscard]] Result<size_t> get_file_pointer() const override { return pos_; }
    [[nodiscard]] VoidResult seek(size_t pos) override;
    [[nodiscard]] Result<size_t> length() const override { return length_; }

    /// One byte out of the mapping.
    ///
    /// Defined here because the postings decoders read one byte per call — a
    /// varint byte, a PFor token, a packed-int byte.  A caller that holds this
    /// concrete type (the postings readers do, through
    /// [`PostingDecodingUtil`](lucene_core:posting_decoding_util)) inlines the
    /// read; the diagnostics of `check_range` (a `std::format` per failure) stay
    /// off that path.
    [[gnu::always_inline]] [[nodiscard]] Result<uint8_t> read_byte() override {
        if (const uint8_t *const pointer = pointer_for(pos_, 1); pointer != nullptr) {
            ++pos_;
            return *pointer;
        }
        return slow_read_byte();
    }
    /// Decoded straight out of the mapping: the default walks one virtual
    /// `read_byte` per byte, and the skip lists of the postings decoders read one
    /// varint at a time.  Inline so that a caller holding the concrete type (the
    /// postings decoders do, through `PostingDecodingUtil`) reads the bytes
    /// without a call.
    [[nodiscard]] Result<int32_t> read_vint() override {
        const size_t available = pos_ <= length_ ? std::min<size_t>(length_ - pos_, 5) : 0;
        if (available >= 1) {
            if (const uint8_t *const pointer = mapping_->ptr_at(offset_ + pos_, available); pointer != nullptr) {
                int32_t value = 0;
                int32_t shift = 0;
                for (size_t used = 0; used < available; ++used) {
                    const int32_t byte = pointer[used];
                    value |= static_cast<int32_t>(static_cast<uint32_t>(byte & 0x7F) << shift);
                    if ((byte & 0x80) == 0) {
                        pos_ += used + 1;
                        return value;
                    }
                    shift += 7;
                }
            }
        }
        // Straddling a chunk boundary, or a malformed varint: the generic decoder
        // reports it.
        return DataInput::read_vint();
    }

    [[nodiscard]] Result<int64_t> read_vlong() override {
        const size_t available = pos_ <= length_ ? std::min<size_t>(length_ - pos_, 10) : 0;
        if (available >= 1) {
            if (const uint8_t *const pointer = mapping_->ptr_at(offset_ + pos_, available); pointer != nullptr) {
                int64_t value = 0;
                int64_t shift = 0;
                for (size_t used = 0; used < available; ++used) {
                    const int64_t byte = pointer[used];
                    value |= (byte & 0x7F) << shift;
                    if ((byte & 0x80) == 0) {
                        pos_ += used + 1;
                        return value;
                    }
                    shift += 7;
                }
            }
        }
        return DataInput::read_vlong();
    }
    [[gnu::always_inline]] [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override {
        if (b.empty()) {
            return {};
        }
        if (auto r = check_range(pos_, b.size()); !r.has_value()) {
            return r;
        }
        if (const auto span = mapping_->contiguous_span(offset_ + pos_, b.size()); span.has_value()) {
            std::memcpy(b.data(), span->data(), b.size());
        } else {
            mapping_->read(offset_ + pos_, b);
        }
        pos_ += b.size();
        return {};
    }
    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override;

    [[nodiscard]] Result<int16_t> read_short() override;
    [[nodiscard]] Result<int32_t> read_int() override;
    [[nodiscard]] Result<int64_t> read_long() override;
    [[nodiscard]] VoidResult read_ints(std::span<int32_t> dst, size_t offset, size_t len) override;

    [[nodiscard]] Result<std::unique_ptr<IndexInput>> slice(std::string_view slice_description,
                                                            size_t offset,
                                                            size_t length) const override;
    [[nodiscard]] Result<std::unique_ptr<RandomAccessInput>> random_access_slice(size_t offset,
                                                                                size_t length) const override;
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> clone() const override;

    [[nodiscard]] VoidResult prefetch(size_t pos, size_t len) override;
    [[nodiscard]] VoidResult close() override;

    [[nodiscard]] std::string to_string() const override;

    // ---- RandomAccessInput (absolute positions) ----

    [[nodiscard]] Result<uint8_t> read_byte(size_t pos) const override;
    [[nodiscard]] Result<int16_t> read_short(size_t pos) const override;
    [[nodiscard]] Result<int32_t> read_int(size_t pos) const override;
    [[nodiscard]] Result<int64_t> read_long(size_t pos) const override;
    [[nodiscard]] VoidResult read_bytes(size_t pos, std::span<uint8_t> bytes) const override;
    [[nodiscard]] std::optional<std::span<const uint8_t>> contiguous_bytes() const override;

private:
    /// [`read_byte`] when the byte is not directly addressable.
    [[nodiscard]] Result<uint8_t> slow_read_byte();

    [[nodiscard]] VoidResult check_range(size_t pos, size_t num_bytes) const;

    /// `num_bytes` bytes at `pos`, when they lie inside this slice and inside one
    /// mapping chunk; `nullptr` otherwise, so the caller can report the error.
    [[nodiscard]] const uint8_t *pointer_for(size_t pos, size_t num_bytes) const noexcept {
        if (pos > length_ || num_bytes > length_ - pos) {
            return nullptr;
        }
        return mapping_->ptr_at(offset_ + pos, num_bytes);
    }

    std::shared_ptr<MemoryMapping> mapping_;
    size_t offset_{0};
    size_t length_{0};
    size_t pos_{0};
};

/// Absolute-position read-only view over a [`MemoryMapping`] region.
///
/// Returned by [`MemorySegmentIndexInput::random_access_slice`]; instances are
/// stateless and therefore thread-safe.
export class MemorySegmentRandomAccessInput final : public RandomAccessInput {
public:
    MemorySegmentRandomAccessInput(std::shared_ptr<MemoryMapping> mapping, size_t offset, size_t length);

    [[nodiscard]] Result<uint8_t> read_byte(size_t pos) const override;
    [[nodiscard]] Result<int16_t> read_short(size_t pos) const override;
    [[nodiscard]] Result<int32_t> read_int(size_t pos) const override;
    [[nodiscard]] Result<int64_t> read_long(size_t pos) const override;
    [[nodiscard]] VoidResult read_bytes(size_t pos, std::span<uint8_t> bytes) const override;
    [[nodiscard]] std::optional<std::span<const uint8_t>> contiguous_bytes() const override;
    [[nodiscard]] Result<size_t> length() const override { return length_; }

private:
    [[nodiscard]] VoidResult check_range(size_t pos, size_t num_bytes) const;

    /// [`MemorySegmentIndexInput::pointer_for`], for this view.
    [[nodiscard]] const uint8_t *pointer_for(size_t pos, size_t num_bytes) const noexcept {
        if (pos > length_ || num_bytes > length_ - pos) {
            return nullptr;
        }
        return mapping_->ptr_at(offset_ + pos, num_bytes);
    }

    std::shared_ptr<MemoryMapping> mapping_;
    size_t offset_{0};
    size_t length_{0};
};

} // namespace lucene
