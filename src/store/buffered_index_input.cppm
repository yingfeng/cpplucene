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

export module lucene_core:buffered_index_input;

import :types;
import :error;

import std;

import :data_input;
import :index_input;
import :io_context;
import :random_access_input;

namespace lucene {

/// An [`IndexInput`] that caches a fixed-size window of the underlying input.
///
/// Port of `buffered_index_input.rs`.  The wrapped input is only ever read
/// through *positional* reads, so the cache never has to seek it.
///
/// Deviations from the reference implementation:
///  * The reference is generic over the sub-input type; here the sub-input is
///    any [`IndexInput`] and positional reads go through its
///    [`IndexInput::random_access_slice`] view (which for file inputs is a
///    `pread` and for memory inputs a `memcpy`).
///  * The cache is byte granular; the reference refills with
///    `buffer_size - unaligned` bytes to keep wider-than-byte reads aligned.
///    Because every scalar read here goes through the byte cache, the
///    unaligned bookkeeping is not needed.
///  * Sequential methods do not lock (matching the [`IndexInput`] contract of
///    "not thread-safe, clone instead").  The absolute
///    [`RandomAccessInput`] methods *do* lock, so one instance may be shared
///    between threads for positional reads -- but a single instance must not
///    be used sequentially on one thread and positionally on another.
export class BufferedIndexInput final : public IndexInput, public RandomAccessInput {
public:
    /// Default buffer size.
    static constexpr size_t BUFFER_SIZE = 1024;
    /// Buffer size used for merges; larger to amortize sequential scans.
    static constexpr size_t MERGE_BUFFER_SIZE = 4096;
    /// Smallest accepted buffer size.
    static constexpr size_t MIN_BUFFER_SIZE = 8;

    /// Takes ownership of `sub` and of `source`, which must be a positional
    /// view over the first `file_length` bytes of `sub`.
    BufferedIndexInput(std::unique_ptr<IndexInput> sub,
                       std::unique_ptr<RandomAccessInput> source,
                       std::string resource_description,
                       size_t buffer_size,
                       size_t file_length);

    ~BufferedIndexInput() override;
    BufferedIndexInput(const BufferedIndexInput &) = delete;
    BufferedIndexInput &operator=(const BufferedIndexInput &) = delete;

    // Hide nothing from the base classes.
    using DataInput::read_bytes;

    /// Buffer size for `context`: `MERGE_BUFFER_SIZE` for merges, otherwise
    /// `BUFFER_SIZE`.
    [[nodiscard]] static size_t buffer_size_for(const IOContext &context);

    /// Validates `buffer_size` (at least [`MIN_BUFFER_SIZE`]).
    [[nodiscard]] static VoidResult check_buffer_size(size_t buffer_size);

    [[nodiscard]] size_t buffer_size() const noexcept { return buffer_size_; }
    /// Bytes currently cached (0 after a seek outside the cache).
    [[nodiscard]] size_t buffered_length() const noexcept { return length_; }
    /// Absolute file offset of `buffer_[0]`.
    [[nodiscard]] size_t buffer_start() const noexcept { return buffer_start_; }

    // ---- IndexInput ----

    [[nodiscard]] Result<size_t> get_file_pointer() const override { return pos_; }
    [[nodiscard]] VoidResult seek(size_t pos) override;
    [[nodiscard]] Result<size_t> length() const override;

    [[nodiscard]] Result<uint8_t> read_byte() override;
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override;
    [[nodiscard]] VoidResult read_bytes_with_buffer(std::span<uint8_t> b, bool use_buffer) override;
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

private:
    /// Copies `dst.size()` bytes at absolute `pos`, filling the cache as
    /// needed.  Mutates the cache, so callers own the instance.
    [[nodiscard]] VoidResult read_at(size_t pos, std::span<uint8_t> dst) const;
    /// [`read_at`] under `mutex_`.
    [[nodiscard]] VoidResult read_at_locked(size_t pos, std::span<uint8_t> dst) const;
    /// Moves the cache so that absolute `pos` is the start of the window.
    [[nodiscard]] VoidResult fill_at(size_t pos) const;

    std::unique_ptr<IndexInput> sub_;
    std::unique_ptr<RandomAccessInput> source_;
    size_t buffer_size_;
    size_t file_length_;
    size_t pos_{0};

    mutable std::vector<uint8_t> buffer_;
    mutable size_t buffer_start_{0};
    mutable size_t length_{0};
    mutable std::mutex mutex_;
};

/// Wraps `sub` with the default buffer size.
[[nodiscard]] Result<std::unique_ptr<BufferedIndexInput>> open_buffered(std::unique_ptr<IndexInput> sub,
                                                                       std::string resource_description);

/// Wraps `sub` with an explicit buffer size.
[[nodiscard]] Result<std::unique_ptr<BufferedIndexInput>> open_buffered_with_buffer_size(
    std::unique_ptr<IndexInput> sub,
    std::string resource_description,
    size_t buffer_size);

/// Wraps `sub` with the buffer size implied by `context`.
[[nodiscard]] Result<std::unique_ptr<BufferedIndexInput>> open_buffered_with_context(
    std::unique_ptr<IndexInput> sub,
    std::string resource_description,
    const IOContext &context);

} // namespace lucene
