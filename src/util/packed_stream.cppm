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

export module lucene_core:packed_stream;

import :types;
import std;

import :data_input;
import :data_output;
import :error;

namespace lucene {

/// Helpers for the two packed-integer layouts the codecs write **without** a
/// header, i.e. as a bare bit stream whose value count and bit width the caller
/// already knows.
///
/// The reference implementation reaches these layouts through
/// `PackedInts::get_writer_no_header` / `get_reader_iterator_no_header`
/// (`PackedFormat::Packed`) and through `DirectReader` over an in-memory
/// `ByteBuffersDataInput`.  This port keeps the byte layouts but spells the
/// helpers as free functions over spans, because the packed-int codec
/// (`:packed_ints`) only covers the header-carrying `DirectWriter`/`DirectReader`
/// pair.
///
/// Two layouts are involved, and they are **not** interchangeable:
///
///  * [`write_packed_stream`] / [`read_packed_stream`] — `Format.PACKED`:
///    a big-endian, MSB-first bit stream, `ceil(count * bits / 8)` bytes.
///  * [`direct_packed_get`] — `DirectWriter`: a little-endian, LSB-first bit
///    stream, plus up to 3 trailing padding bytes that this reader ignores.

/// Number of bytes a `Format.PACKED` stream of `value_count` values occupies.
[[nodiscard]] size_t packed_stream_byte_count(size_t value_count, int32_t bits_per_value) noexcept;

/// Writes `values` as a `Format.PACKED` bit stream (MSB first, zero padded to a
/// whole byte).  `bits_per_value == 0` writes nothing.
[[nodiscard]] VoidResult write_packed_stream(DataOutput &out, std::span<const int64_t> values,
                                             int32_t bits_per_value);

/// Reads `value_count` values of a `Format.PACKED` bit stream.
[[nodiscard]] Result<std::vector<int64_t>> read_packed_stream(DataInput &in, size_t value_count,
                                                              int32_t bits_per_value);

/// Reads the value at `index` of a `DirectWriter` bit stream held in memory.
///
/// `bytes` starts at the first byte of the packed region; the little-endian
/// LSB-first layout is the same one `:direct_reader` decodes through a
/// [`RandomAccessInput`], only here the storage is a plain buffer.  Trailing
/// padding bytes are never touched.
[[nodiscard]] Result<int64_t> direct_packed_get(std::span<const uint8_t> bytes, int32_t bits_per_value,
                                                size_t index);

} // namespace lucene
