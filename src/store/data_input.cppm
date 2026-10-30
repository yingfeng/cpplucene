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
    /// Fills `dst[offset, offset + len)` with `len` little-endian ints.
    ///
    /// The default implementation issues one virtual [`read_int`] per value.
    /// Inputs whose `read_bytes` copies a run in one go override this with
    /// [`read_ints_bulk`], which is what the postings decoders see.
    [[nodiscard]] virtual VoidResult read_ints(std::span<int32_t> dst, size_t offset, size_t len);
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

/// Fills `dst[offset, offset + len)` with `len` little-endian ints, read through a
/// single [`DataInput::read_bytes`] call.
///
/// [`DataInput::read_ints`] is a per-value virtual `read_int()` loop, which costs a
/// virtual call plus a range check per int.  For inputs whose `read_bytes` copies a
/// run in one go (`ByteArrayDataInput`, `BufferedIndexInput`, `SlicedIndexInput`,
/// `MemorySegmentIndexInput`) one bulk fill followed by a byte-order fixup is far
/// cheaper, and the postings decoders call `read_ints` on every block.
///
/// The caller must not use this on an input whose `read_bytes` is itself a
/// byte-at-a-time loop; those are no faster (and no slower) than the default.
export [[nodiscard]] VoidResult read_ints_bulk(DataInput &input,
                                               std::span<int32_t> dst,
                                               size_t offset,
                                               size_t len);

/// [`DataInput`] backed by a byte array.  Port of `ByteArrayDataInput`.
///
/// The input never copies the bytes; the caller keeps the buffer alive.
export class ByteArrayDataInput final : public DataInput {
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

    /// The three byte-level readers are defined here rather than out of line:
    /// this class is `final`, so a caller holding a `ByteArrayDataInput &` (the
    /// impacts and terms decoders do) resolves them statically, and the body is
    /// then a bounds check and a load instead of a call.
    [[gnu::always_inline]] [[nodiscard]] Result<uint8_t> read_byte() override {
        const size_t pos = pos_;
        if (pos >= limit_) {
            return err(LuceneError::eof("read past eof"));
        }
        pos_ = pos + 1;
        return bytes_[pos];
    }

    /// Decoded from the buffer directly: the default walks one virtual
    /// `read_byte` per byte, which the postings decoders pay for on every skip
    /// list entry.
    [[gnu::always_inline]] [[nodiscard]] Result<int32_t> read_vint() override {
        int32_t value = 0;
        int32_t shift = 0;
        size_t position = pos_;
        while (position < limit_ && shift < 35) {
            const int32_t byte = bytes_[position++];
            value |= static_cast<int32_t>(static_cast<uint32_t>(byte & 0x7F) << shift);
            if ((byte & 0x80) == 0) {
                pos_ = position;
                return value;
            }
            shift += 7;
        }
        // Malformed or truncated: let the generic decoder report it.
        return DataInput::read_vint();
    }

    [[gnu::always_inline]] [[nodiscard]] Result<int64_t> read_vlong() override {
        int64_t value = 0;
        int64_t shift = 0;
        size_t position = pos_;
        while (position < limit_ && shift < 70) {
            const int64_t byte = bytes_[position++];
            value |= (byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                pos_ = position;
                return value;
            }
            shift += 7;
        }
        return DataInput::read_vlong();
    }
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override;
    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override;

    [[nodiscard]] Result<int16_t> read_short() override;
    [[nodiscard]] Result<int32_t> read_int() override;
    [[nodiscard]] Result<int64_t> read_long() override;
    [[nodiscard]] VoidResult read_group_vint(std::span<int32_t> dst) override;
    [[nodiscard]] VoidResult read_ints(std::span<int32_t> dst, size_t offset, size_t len) override;

    [[nodiscard]] std::string to_string() const override;

private:
    [[nodiscard]] Result<std::span<const uint8_t>> slice(size_t len);

    std::span<const uint8_t> bytes_{};
    size_t pos_{0};
    size_t limit_{0};
};

} // namespace lucene
