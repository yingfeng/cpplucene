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

export module lucene_core:packed64;

import :types;
import :error;

import std;

import :packed_ints;

namespace lucene {

/// An in-memory, mutable array of packed integers.
///
/// Port of `util/packed/packed64.rs` and `packed64_single_block.rs`, merged into
/// one class.
///
/// **Deviation:** the reference implementation (like Lucene) chooses between `Packed64` (values
/// packed little-endian inside 64-bit words) and `Packed64SingleBlock`
/// (big-endian inside words, only for bit widths that divide 64) purely as a
/// speed optimization.  This port always uses the little-endian layout, which
/// keeps a single code path; the layout never leaves memory, so no on-disk
/// compatibility depends on it.
export class Packed64 {
public:
    /// Creates an array of `value_count` zeroed values of `bits_per_value` bits.
    [[nodiscard]] static Result<Packed64> create(size_t value_count, int32_t bits_per_value);

    [[nodiscard]] int64_t get(size_t index) const noexcept;
    [[nodiscard]] VoidResult set(size_t index, int64_t value);
    /// Sets `[from_index, to_index)` to `value`.
    [[nodiscard]] VoidResult fill(size_t from_index, size_t to_index, int64_t value);
    /// Copies `[src_pos, src_pos + len)` of `source` into this array at `dest_pos`.
    [[nodiscard]] VoidResult copy_from(const Packed64 &source, size_t src_pos, size_t dest_pos, size_t len);

    void clear() noexcept;

    [[nodiscard]] size_t size() const noexcept { return value_count_; }
    [[nodiscard]] int32_t bits_per_value() const noexcept { return bits_per_value_; }
    /// Bytes held by the backing word array.
    [[nodiscard]] size_t ram_bytes_used() const noexcept { return words_.size() * sizeof(uint64_t); }

private:
    Packed64(size_t value_count, int32_t bits_per_value, std::vector<uint64_t> words);

    [[nodiscard]] uint64_t mask() const noexcept;

    size_t value_count_{0};
    int32_t bits_per_value_{1};
    std::vector<uint64_t> words_;
};

} // namespace lucene
