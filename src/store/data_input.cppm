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

export module lucene_core:data_input;

import :types;
import std;

import :error;

namespace lucene {

/// Base class for performing read operations on Lucene's low-level data types.
///
/// Port of the `DataInput` trait (`core/store/data_input.rs`).  Implementations
/// are not thread-safe: they maintain an internal position.
///
/// Deviations from the reference implementation, both forced by C++:
///  * `get_bytes` returns an owned buffer instead of a `Cow<[u8]>`; hot paths
///    should use `read_bytes(std::span<uint8_t>)` which never allocates.
///  * `read_string` returns an owned `std::string` for the same reason.
export class DataInput {
public:
    virtual ~DataInput() = default;

    /// Reads and returns a single byte.
    [[nodiscard]] virtual Result<uint8_t> read_byte() = 0;

    /// Reads exactly `b.size()` bytes into `b`.
    [[nodiscard]] virtual VoidResult read_bytes(std::span<uint8_t> b) = 0;

    /// Reads `len` bytes into `b` starting at `offset`.
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b, size_t offset, size_t len);

    /// Same as [`read_bytes`] with control over whether the input may buffer.
    [[nodiscard]] virtual VoidResult read_bytes_with_buffer(std::span<uint8_t> b, bool /*use_buffer*/) {
        return read_bytes(b);
    }

    /// Skips over `num_bytes` bytes.
    [[nodiscard]] virtual VoidResult skip_bytes(int64_t num_bytes) = 0;

    /// Reads a group of 4 vints (group-varint encoding) into `dst[0..4]`.
    [[nodiscard]] virtual VoidResult read_group_vint(std::span<int32_t> dst);

    // ---- default implementations ----

    [[nodiscard]] virtual Result<int16_t> read_short();
    [[nodiscard]] virtual Result<int32_t> read_int();
    [[nodiscard]] virtual Result<int64_t> read_long();

    /// Reads an `int32_t` stored in a variable-length format (1 to 5 bytes).
    [[nodiscard]] virtual Result<int32_t> read_vint();
    /// Reads a zig-zag encoded vint.
    [[nodiscard]] Result<int32_t> read_zint();
    /// Reads an `int64_t` stored in a variable-length format (1 to 9 bytes).
    [[nodiscard]] virtual Result<int64_t> read_vlong();
    /// Reads a zig-zag encoded vlong.
    [[nodiscard]] Result<int64_t> read_zlong();

    /// Reads `len` bytes into a freshly allocated buffer.
    [[nodiscard]] Result<std::vector<uint8_t>> get_bytes(size_t len);

    [[nodiscard]] VoidResult read_longs(std::span<int64_t> dst, size_t offset, size_t len);
    [[nodiscard]] VoidResult read_ints(std::span<int32_t> dst, size_t offset, size_t len);
    [[nodiscard]] VoidResult read_floats(std::span<float> dst, size_t offset, size_t len);

    /// Reads a string written by `DataOutput::write_string`.
    [[nodiscard]] Result<std::string> read_string();
    /// Reads a map written by `DataOutput::write_map_of_strings`.
    [[nodiscard]] Result<std::map<std::string, std::string>> read_map_of_strings();
    /// Reads a set written by `DataOutput::write_set_of_strings`.
    [[nodiscard]] Result<std::set<std::string>> read_set_of_strings();

    /// Human readable description, used in error messages.
    [[nodiscard]] virtual std::string to_string() const { return "DataInput"; }
};

/// [`DataInput`] backed by a byte array.  Port of `ByteArrayDataInput`.
///
/// The input never copies the bytes; the caller keeps the buffer alive.
export class ByteArrayDataInput : public DataInput {
public:
    ByteArrayDataInput() = default;
    explicit ByteArrayDataInput(std::span<const uint8_t> bytes);

    [[nodiscard]] static ByteArrayDataInput with_range(std::span<const uint8_t> bytes, size_t offset, size_t length);

    void reset_meta(size_t offset, size_t length);
    void reset_with_range(std::span<const uint8_t> bytes, size_t offset, size_t length);
    void set_position(size_t position) noexcept { pos_ = position; }
    void rewind() noexcept { pos_ = 0; }

    [[nodiscard]] size_t get_position() const noexcept { return pos_; }
    [[nodiscard]] size_t length() const noexcept { return limit_; }
    [[nodiscard]] bool eof() const noexcept { return pos_ == limit_; }
    [[nodiscard]] std::span<const uint8_t> bytes() const noexcept { return bytes_; }

    [[nodiscard]] Result<uint8_t> read_byte() override;
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override;
    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override;

    [[nodiscard]] Result<int16_t> read_short() override;
    [[nodiscard]] Result<int32_t> read_int() override;
    [[nodiscard]] Result<int64_t> read_long() override;
    [[nodiscard]] VoidResult read_group_vint(std::span<int32_t> dst) override;

    [[nodiscard]] std::string to_string() const override;

private:
    [[nodiscard]] Result<std::span<const uint8_t>> slice(size_t len);

    std::span<const uint8_t> bytes_{};
    size_t pos_{0};
    size_t limit_{0};
};

} // namespace lucene
