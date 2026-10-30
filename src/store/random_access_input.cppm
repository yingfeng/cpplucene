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

export module lucene_core:random_access_input;

import :types;

import std;

import :error;

namespace lucene {

/// Random access to a region of a file.  Port of `RandomAccessInput`.
///
/// Unlike [`IndexInput`], every read is absolute, so implementations are
/// stateless and therefore thread-safe.
export class RandomAccessInput {
public:
    virtual ~RandomAccessInput() = default;

    [[nodiscard]] virtual Result<uint8_t> read_byte(size_t pos) const = 0;
    [[nodiscard]] virtual Result<int16_t> read_short(size_t pos) const = 0;
    [[nodiscard]] virtual Result<int32_t> read_int(size_t pos) const = 0;
    [[nodiscard]] virtual Result<int64_t> read_long(size_t pos) const = 0;

    /// Reads `bytes.size()` bytes starting at `pos`.
    [[nodiscard]] virtual VoidResult read_bytes(size_t pos, std::span<uint8_t> bytes) const = 0;

    /// Number of bytes reachable through this view (relative to its own origin).
    [[nodiscard]] virtual Result<size_t> length() const = 0;

    /// The whole view as one contiguous block of memory, when the backing store
    /// is memory mapped and the view does not straddle two mappings.
    ///
    /// **Deviation:** the reference implementation has no such accessor.  Per
    /// value reads out of a memory mapped file (norms, doc values) otherwise pay
    /// a virtual call, a bounds check and a `memcpy` each, which dominates the
    /// scoring loop; taking the bytes once lets a reader index them directly.
    /// Callers must keep `pos + len <= length()`.
    [[nodiscard]] virtual std::optional<std::span<const uint8_t>> contiguous_bytes() const { return std::nullopt; }

    // ---- defaults built on the primitives above ----

    [[nodiscard]] VoidResult read_ints(size_t pos, std::span<int32_t> dst, size_t offset, size_t len) const;
    [[nodiscard]] VoidResult read_longs(size_t pos, std::span<int64_t> dst, size_t offset, size_t len) const;
    [[nodiscard]] VoidResult read_floats(size_t pos, std::span<float> dst, size_t offset, size_t len) const;
};

} // namespace lucene
