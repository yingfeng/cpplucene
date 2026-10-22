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

export module lucene_core:direct_reader;

import :types;
import :error;

import std;

import :packed_ints;
import :random_access_input;

namespace lucene {

/// Reads values written by [`DirectWriter`].
///
/// Port of `util/packed/direct_reader.rs` (Lucene's `DirectReader`).
///
/// **Deviation:** rlucene dispatches to a specialized reader per bit width
/// (`DirectPackedReader1/2/4/12/20/28`, plus a buffered merge reader for
/// decoding 128 values at once).  Here one generic decoder reads exactly the
/// bytes that hold the requested value and reassembles them little endian
/// LSB-first, so it never reads past the packed region.  A specialised fast
/// path can be added later without changing the layout.
export class DirectReader {
public:
    /// Creates a reader for a packed sequence starting at `base_offset`.
    ///
    /// `bits_per_value` must be one of
    /// [`DirectWriter::SUPPORTED_BITS_PER_VALUE`].
    DirectReader(const RandomAccessInput &input, size_t base_offset, int32_t bits_per_value);

    /// Same, with `base_offset == 0`, validating `bits_per_value`.
    [[nodiscard]] static Result<DirectReader> get_instance(const RandomAccessInput &input, int32_t bits_per_value);

    /// The value at `index`.
    [[nodiscard]] Result<int64_t> get(size_t index) const;

    [[nodiscard]] int32_t bits_per_value() const noexcept { return bits_per_value_; }
    [[nodiscard]] size_t base_offset() const noexcept { return base_offset_; }

private:
    const RandomAccessInput *input_;
    size_t base_offset_;
    int32_t bits_per_value_;
};

} // namespace lucene
