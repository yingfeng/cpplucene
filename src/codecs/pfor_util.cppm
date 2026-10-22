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

export module lucene_core:pfor_util;

import :types;
import std;

import :error;
import :data_input;
import :data_output;
import :for_util;
import :long_heap;
import :posting_decoding_util;

namespace lucene {

/// Utility struct to encode sequences of 128 small positive integers.
/// Port of `PForUtil` (`core/codecs/lucene101/pfor_util.rs`).
///
/// Patched frame-of-reference: at most [`MAX_EXCEPTIONS`] values that do not
/// fit in `patched_bits_required` bits are written out as `(index, patch)`
/// byte pairs after the block.  A token byte stores the number of exceptions
/// in the top 3 bits and the bits per value in the low 5 bits; a bits-per-value
/// of 0 means "all values are equal" and the single value follows as a vint.
export class PForUtil {
public:
    static constexpr size_t MAX_EXCEPTIONS = 7;

    /// True when `arr[0..BLOCK_SIZE)` only contains the value `arr[0]`.
    [[nodiscard]] static bool all_equal(std::span<const int32_t> arr);

    /// Encodes the 128 integers of `ints` into `out`.  `ints` is modified in
    /// place: exception values are truncated to `patched_bits_required` bits.
    [[nodiscard]] VoidResult encode(std::span<int32_t> ints, DataOutput &out);

    /// Decodes 128 integers into `ints`.
    [[nodiscard]] VoidResult decode(PostingDecodingUtil &pdu, std::span<int32_t> ints);

    /// Skips 128 integers.
    [[nodiscard]] static VoidResult skip(DataInput &input);

private:
    ForUtil for_util_;
    std::optional<LongHeap> top_;
};

} // namespace lucene
