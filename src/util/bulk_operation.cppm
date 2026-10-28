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

export module lucene_core:bulk_operation;

import :types;
import std;

namespace lucene {

/// Bulk encoding and decoding of `bits_per_value`-bit values packed MSB first.
///
/// Port of the `BulkOperation` family (`util/packed/bulk_operation*.rs`).  The
/// reference implementation ships one generated class per width, because a Rust
/// const generic cannot specialise the bit arithmetic; C++ can, so there is a
/// single template instantiated for every width from 1 to 64.
///
/// The layout is the one [`write_packed_stream`](:packed_stream) uses: the values
/// are concatenated most-significant bit first, and a value may straddle byte
/// boundaries.
export class BulkOperation {
public:
    virtual ~BulkOperation() = default;

    /// Decodes `values.size()` values starting at the `offset`-th value of
    /// `blocks`.
    ///
    /// `blocks` may be shorter than the last window needs; the missing bytes are
    /// read as zero, which is what a caller that only owns the exact byte range
    /// wants.
    virtual void decode(std::span<const uint8_t> blocks, size_t offset, std::span<int64_t> values) const = 0;

    /// Encodes `values` into `blocks`, starting at the `offset`-th value.
    ///
    /// The bits of every value outside the values written are preserved, so a
    /// caller can encode a batch that starts in the middle of a byte.
    virtual void encode(std::span<int64_t> values, size_t offset, std::span<uint8_t> blocks) const = 0;

    /// How many bytes `count` values take.
    [[nodiscard]] virtual size_t byte_count(size_t count) const noexcept = 0;

    /// How many bits a value has.
    [[nodiscard]] virtual int32_t bits_per_value() const noexcept = 0;
};

/// The bulk operation for `bits_per_value` bits, from 1 to 64.
///
/// The result is a reference to a shared instance, so callers do not own it.
[[nodiscard]] const BulkOperation &bulk_operation(int32_t bits_per_value);

} // namespace lucene
