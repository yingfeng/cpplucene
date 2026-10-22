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

export module lucene_core:index_input;

import :types;
import :debug;
import :error;

import std;

import :data_input;
import :random_access_input;
import :io_context;

namespace lucene {

/// Abstract base class for reading from a file in a [`Directory`].
///
/// Port of the `IndexInput` trait (`core/store/index_input.rs`).
///
/// Deviations from rlucene (C++ mechanics only):
///  * `clone()` and `slice()` return `std::unique_ptr<IndexInput>` instead of an
///    associated type, so no type erasure enum is needed.
///  * `close()` is explicit but harmless to call twice; there is no `Drop`
///    based closing because the owning `Directory` keeps the file alive.
///
/// Thread safety: instances are **not** thread-safe, clone them instead.
export class IndexInput : public DataInput {
public:
    /// The position of the next read.
    [[nodiscard]] virtual Result<size_t> get_file_pointer() const = 0;

    /// Sets the position of the next read.
    [[nodiscard]] virtual VoidResult seek(size_t pos) = 0;

    /// Number of bytes covered by this input.
    [[nodiscard]] virtual Result<size_t> length() const = 0;

    /// A new, independent input over `[offset, offset + length)` positioned at 0.
    [[nodiscard]] virtual Result<std::unique_ptr<IndexInput>> slice(std::string_view slice_description,
                                                                   size_t offset,
                                                                   size_t length) const;

    /// Same as [`slice`] but with an explicit read advice.
    [[nodiscard]] virtual Result<std::unique_ptr<IndexInput>> slice_with_read_advice(std::string_view slice_description,
                                                                                   size_t offset,
                                                                                   size_t length,
                                                                                   ReadAdvice /*read_advice*/) const {
        return slice(slice_description, offset, length);
    }

    /// An independent random-access view over `[offset, offset + length)`.
    [[nodiscard]] virtual Result<std::unique_ptr<RandomAccessInput>> random_access_slice(size_t offset,
                                                                                        size_t length) const;

    /// Hint that `[pos, pos + len)` will be read soon.
    [[nodiscard]] virtual VoidResult prefetch(size_t /*pos*/, size_t /*len*/) { return {}; }

    /// Releases the underlying resource. Idempotent.
    [[nodiscard]] virtual VoidResult close() { return {}; }

    /// A new input over the same file with an independent position.
    [[nodiscard]] virtual Result<std::unique_ptr<IndexInput>> clone() const = 0;

    // ---- DataInput defaults that prefer positional reads ----

    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override;

    /// Human readable description used by error messages (`slice` chains).
    [[nodiscard]] std::string to_string() const override { return description_.empty() ? "IndexInput" : description_; }
    [[nodiscard]] const std::string &description() const noexcept { return description_; }

protected:
    void set_description(std::string description) { description_ = std::move(description); }

private:
    std::string description_{};
};

/// An [`IndexInput`] view over `[offset, offset + length)` of another input.
///
/// This is what `slice()` returns when the concrete input has no cheaper way to
/// express a sub-range (for example a file-backed input that is not memory
/// mapped).
export class SlicedIndexInput : public IndexInput {
public:
    /// Takes ownership of `parent`, which must be positioned anywhere.
    SlicedIndexInput(std::unique_ptr<IndexInput> parent, size_t offset, size_t length, std::string description);

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

private:
    [[nodiscard]] VoidResult prepare_read() const;

    std::unique_ptr<IndexInput> parent_;
    size_t offset_{0};
    size_t length_{0};
    size_t pos_{0};
};

/// Adapts an [`IndexInput`] to [`RandomAccessInput`] through seek + read.
///
/// Used when the underlying storage offers no true random access.  Each call is
/// serialized, so the object is safe to share between threads.
export class SeekReadRandomAccessInput : public RandomAccessInput {
public:
    explicit SeekReadRandomAccessInput(std::unique_ptr<IndexInput> input, size_t offset, size_t length);

    [[nodiscard]] Result<uint8_t> read_byte(size_t pos) const override;
    [[nodiscard]] Result<int16_t> read_short(size_t pos) const override;
    [[nodiscard]] Result<int32_t> read_int(size_t pos) const override;
    [[nodiscard]] Result<int64_t> read_long(size_t pos) const override;
    [[nodiscard]] VoidResult read_bytes(size_t pos, std::span<uint8_t> bytes) const override;
    [[nodiscard]] Result<size_t> length() const override { return length_; }

private:
    [[nodiscard]] Result<size_t> check(size_t pos, size_t num_bytes) const;

    mutable std::unique_ptr<IndexInput> input_;
    mutable std::mutex mutex_;
    size_t offset_{0};
    size_t length_{0};
};

} // namespace lucene
