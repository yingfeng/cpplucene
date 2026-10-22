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

export module lucene_core:group_vint_util;

import :types;
import std;

import :error;
import :bit_util;
import :data_input;
import :data_output;

namespace lucene {

/// Provides an abstraction for reading `int32_t` values at an absolute
/// position, so that decoding logic can be reused with different
/// [`DataInput`] implementations.  Port of the `IntReader` trait
/// (`core/util/group_vint_util.rs`).
export class IntReader {
public:
    virtual ~IntReader() = default;

    /// Reads the `int32_t` stored at `pos` without moving this reader's own
    /// position when the implementation is random-access.
    [[nodiscard]] virtual Result<int32_t> read(size_t pos) = 0;
};

/// Utility methods and constants for group-varint encoding.  Port of
/// `GroupVIntUtil` (`core/util/group_vint_util.rs`).
///
/// The tail groups (fewer than 4 values) are encoded with plain vints, which
/// is why every read/write entry point takes an explicit `limit`.
export struct GroupVIntUtil {
    /// Decode into `uint64_t` before narrowing so negative `int32_t` bit
    /// patterns can be represented as positive values.
    static constexpr std::array<uint64_t, 4> LONG_MASKS = {0xFFull, 0xFFFFull, 0xFFFFFFull, 0xFFFFFFFFull};
    static constexpr std::array<int32_t, 4> INT_MASKS = {0xFF, 0xFFFF, 0xFFFFFF, -1};

    /// The maximum length of a single group-varint is 4 integers + 1 byte flag.
    static constexpr size_t MAX_LENGTH_PER_GROUP = 17;

    /// Reads all the group varints, including the tail vints.
    ///
    /// `dst` must hold at least `limit` values; values are written in
    /// `dst[0..limit)`.
    [[nodiscard]] static VoidResult read_group_vints_i64(DataInput &input, std::span<int64_t> dst, size_t limit);

    /// Reads all the group varints, including the tail vints.
    [[nodiscard]] static VoidResult read_group_vints_i32(DataInput &input, std::span<int32_t> dst, size_t limit);

    /// Reads a single group of 4 values into `dst[offset..offset + 4)`.
    [[nodiscard]] static VoidResult read_group_vint_i64(DataInput &input, std::span<int64_t> dst, size_t offset);
    /// Reads a single group of 4 values into `dst[offset..offset + 4)`.
    [[nodiscard]] static VoidResult read_group_vint_i32(DataInput &input, std::span<int32_t> dst, size_t offset);

    /// Faster implementation of reading a single group: it pulls each value
    /// with `IntReader::read` so that no bounds checks happen per byte.
    ///
    /// Falls back to [`read_group_vint_i64`] when `remaining` is smaller than
    /// [`MAX_LENGTH_PER_GROUP`], in which case `0` is returned.
    ///
    /// Returns the number of bytes consumed excluding the flag byte, i.e. a
    /// value in `[0, MAX_LENGTH_PER_GROUP - 1]`.
    ///
    /// The reference implementation expresses the "same object is both a `DataInput` and an
    /// `IntReader`" requirement with a trait bound (`DI: DataInput + IntReader`).
    /// C++ has no such bound here — `IntReader` cannot live in `:data_input`
    /// because it is where the group-varint decoder lives — so `reader` and
    /// `input` are passed separately and must refer to the same object.
    [[nodiscard]] static Result<size_t> read_group_vint_i64_with_reader(DataInput &input,
                                                                       IntReader &reader,
                                                                       uint64_t remaining,
                                                                       size_t pos,
                                                                       std::span<int64_t> dst,
                                                                       size_t offset);

    /// `int32_t` counterpart of [`read_group_vint_i64_with_reader`].
    [[nodiscard]] static Result<size_t> read_group_vint_i32_with_reader(DataInput &input,
                                                                       IntReader &reader,
                                                                       size_t remaining,
                                                                       size_t pos,
                                                                       std::span<int32_t> dst,
                                                                       size_t offset);

    /// Writes `values[0..limit)` as group-varints.
    ///
    /// `scratch` must provide at least [`MAX_LENGTH_PER_GROUP`] bytes; it is
    /// used to assemble one group before it is handed to `data_output`.
    [[nodiscard]] static VoidResult write_group_vints_i64(DataOutput &data_output,
                                                         std::span<uint8_t> scratch,
                                                         std::span<const int64_t> values,
                                                         size_t limit);

    /// Writes `values[0..limit)` as group-varints.
    [[nodiscard]] static VoidResult write_group_vints_i32(DataOutput &data_output,
                                                         std::span<uint8_t> scratch,
                                                         std::span<const int32_t> values,
                                                         size_t limit);

private:
    /// Number of bytes needed to store `v` (1 when `v == 0`).
    [[nodiscard]] static size_t num_bytes(int32_t v);
    /// Narrows an `int64_t` to `int32_t`, rejecting values outside `[0, 0xFFFFFFFF]`.
    [[nodiscard]] static Result<int32_t> to_int(int64_t value);
    /// Reads a value stored with `num_bytes_minus1 + 1` little-endian bytes.
    [[nodiscard]] static Result<int32_t> read_int_in_group(DataInput &input, size_t num_bytes_minus1);
};

} // namespace lucene
