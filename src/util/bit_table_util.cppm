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

export module lucene_core:bit_table_util;

import :types;
import :error;

import std;

import :fst_bytes_reader;

namespace lucene {

/// Number of bytes needed to store a presence bit table for `num_arcs` labels.
export inline constexpr int32_t get_num_presence_bytes(int32_t num_arcs) noexcept {
    return (num_arcs + 7) / 8;
}

/// Operations on the presence bit table of a direct-addressing FST node.
///
/// Port of `util/fst_impl/bit_table_util.rs`.  Bits are numbered little endian:
/// bit `i` lives in byte `i >> 3` at `1 << (i & 7)`.  Every entry point expects
/// the reader to be positioned at the beginning of the bit table.
export struct BitTableUtil {
    /// Whether the bit at `bit_index` is set.
    [[nodiscard]] static Result<bool> is_bit_set(int32_t bit_index, FstBytesReader &reader);

    /// Counts the set bits in a bit table of `bit_table_bytes` bytes.
    [[nodiscard]] static Result<int32_t> count_bits(int32_t bit_table_bytes, FstBytesReader &reader);

    /// Counts the set bits before `bit_index` (exclusive).
    [[nodiscard]] static Result<int32_t> count_bits_upto(int32_t bit_index, FstBytesReader &reader);

    /// Index of the first set bit at or after `bit_index`, or `-1`.
    [[nodiscard]] static Result<int32_t> next_bit_set(int32_t bit_index, int32_t num_bytes, FstBytesReader &reader);

    /// Index of the last set bit strictly before `bit_index`, or `-1`.
    [[nodiscard]] static Result<int32_t> previous_bit_set(int32_t bit_index, FstBytesReader &reader);
};

} // namespace lucene
