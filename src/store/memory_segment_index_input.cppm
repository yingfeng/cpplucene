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

    /// Advises the kernel that `[pos, pos + len)` will be read soon.
    void prefetch(size_t pos, size_t len) const noexcept;

private:
    std::vector<uint8_t *> segments_;
    std::vector<size_t> segment_sizes_;
    size_t chunk_size_{0};
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

    [[nodiscard]] Result<uint8_t> read_byte() override;
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override;
    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override;

    [[nodiscard]] Result<int16_t> read_short() override;
    [[nodiscard]] Result<int32_t> read_int() override;
    [[nodiscard]] Result<int64_t> read_long() override;

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

private:
    [[nodiscard]] VoidResult check_range(size_t pos, size_t num_bytes) const;

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
    [[nodiscard]] Result<size_t> length() const override { return length_; }

private:
    [[nodiscard]] VoidResult check_range(size_t pos, size_t num_bytes) const;

    std::shared_ptr<MemoryMapping> mapping_;
    size_t offset_{0};
    size_t length_{0};
};

} // namespace lucene
