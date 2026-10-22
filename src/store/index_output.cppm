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

export module lucene_core:index_output;

import :types;
import :error;

import std;

import :data_output;

namespace lucene {

/// Abstract base class for writing to a file in a [`Directory`].
///
/// Port of the `IndexOutput` trait (`core/store/index_output.rs`).  The output
/// keeps a running CRC-32 over everything written, which the compound file
/// format and the segment checksums rely on.
export class IndexOutput : public DataOutput {
public:
    /// The position of the next write.
    [[nodiscard]] virtual Result<size_t> get_file_pointer() const = 0;

    /// CRC-32 of everything written so far.
    [[nodiscard]] virtual uint64_t get_checksum() = 0;

    /// File name this output writes to (`""` for temporary outputs).
    [[nodiscard]] virtual std::string_view get_name() const = 0;

    /// Flushes and closes the underlying file.  Must not be called twice.
    [[nodiscard]] virtual VoidResult close() = 0;

    /// Writes zero bytes until the file pointer is a multiple of
    /// `alignment_bytes`.  Returns the number of padding bytes written.
    [[nodiscard]] virtual Result<size_t> align_file_pointer(size_t alignment_bytes);

    /// Alias kept for readability in ported code: `write_int_le`.
    [[nodiscard]] VoidResult write_int_le(int32_t i) { return write_int(i); }
    /// Alias kept for readability in ported code: `write_long_le`.
    [[nodiscard]] VoidResult write_long_le(int64_t i) { return write_long(i); }
};

} // namespace lucene
