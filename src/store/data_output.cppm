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

export module clucene:data_output;

import :types;
import std;

import :error;
import :data_input;

namespace clucene {

/// Base class for performing write operations on Lucene's low-level data types.
///
/// Port of the `DataOutput` trait (`core/store/data_output.rs`).
/// Not thread-safe: implementations maintain an internal position.
export class DataOutput {
public:
    virtual ~DataOutput() = default;

    /// Writes a single byte.
    [[nodiscard]] virtual VoidResult write_byte(uint8_t b) = 0;

    /// Writes all of `b`.
    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b) { return write_bytes(b, 0, b.size()); }

    /// Writes `length` bytes of `b` starting at `offset`.
    [[nodiscard]] virtual VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) = 0;

    // ---- default implementations ----

    [[nodiscard]] virtual VoidResult write_int(int32_t i);
    [[nodiscard]] virtual VoidResult write_short(int16_t i);
    [[nodiscard]] virtual VoidResult write_long(int64_t i);

    /// Writes an `int32_t` in a variable-length format (1 to 5 bytes).
    [[nodiscard]] virtual VoidResult write_vint(int32_t i);
    /// Writes a zig-zag encoded vint.
    [[nodiscard]] VoidResult write_zint(int32_t i);
    /// Writes an `int64_t` in a variable-length format (1 to 9 bytes).
    [[nodiscard]] virtual VoidResult write_vlong(int64_t i);
    /// Writes a zig-zag encoded vlong.
    [[nodiscard]] VoidResult write_zlong(int64_t i);

    /// Writes `s` as `vint(byteLength)` followed by the UTF-8 bytes.
    [[nodiscard]] virtual VoidResult write_string(std::string_view s);

    /// Copies `num_bytes` bytes from `input` to this output.
    [[nodiscard]] virtual VoidResult copy_bytes(DataInput &input, size_t num_bytes);

    /// Writes a map as `vint(size)` followed by key/value string pairs.
    [[nodiscard]] VoidResult write_map_of_strings(const std::map<std::string, std::string> &map);
    /// Writes a set as `vint(size)` followed by the values.
    [[nodiscard]] VoidResult write_set_of_strings(const std::set<std::string> &set);

    /// Human readable description, used in error messages.
    [[nodiscard]] virtual std::string to_string() const { return "DataOutput"; }

protected:
    /// Writes a vlong allowing negative values (used by `write_zlong`).
    [[nodiscard]] VoidResult write_signed_vlong(int64_t i);
};

/// [`DataOutput`] backed by a pre-allocated byte array, mirroring
/// `ByteArrayDataOutput`.  The caller owns the storage; writes beyond `limit`
/// are rejected.
export class ByteArrayDataOutput : public DataOutput {
public:
    ByteArrayDataOutput() = default;
    explicit ByteArrayDataOutput(std::span<uint8_t> bytes);

    [[nodiscard]] static ByteArrayDataOutput with_range(std::span<uint8_t> bytes, size_t offset, size_t length);

    void reset();
    void reset_with_range(size_t offset, size_t length) noexcept;

    [[nodiscard]] size_t get_position() const noexcept { return pos_; }
    [[nodiscard]] size_t limit() const noexcept { return limit_; }

    [[nodiscard]] VoidResult write_byte(uint8_t b) override;
    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override;

    [[nodiscard]] VoidResult write_int(int32_t i) override;
    [[nodiscard]] VoidResult write_short(int16_t i) override;
    [[nodiscard]] VoidResult write_long(int64_t i) override;

    [[nodiscard]] std::string to_string() const override;

private:
    [[nodiscard]] VoidResult ensure_room(size_t num_bytes) const;

    std::span<uint8_t> bytes_{};
    size_t pos_{0};
    size_t limit_{0};
};

} // namespace clucene
