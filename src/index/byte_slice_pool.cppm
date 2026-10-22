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

export module lucene_core:byte_slice_pool;

import :types;
import std;

import :byte_block_pool;
import :error;

namespace lucene {

/// Allocates the interleaved byte slices that the indexing chain writes into a
/// shared [`ByteBlockPool`].
///
/// Port of `index/byte_slice_pool.rs`.  Slices grow through a fixed ladder of
/// levels (5, 14, 20, ... bytes).  The last 4 bytes of a non-final slice hold
/// the flat offset of the next slice ("forwarding address"), and the last byte
/// of every slice encodes its level in the low 4 bits with `0x10` as the
/// end-of-slice marker, so writers never have to store a length.
export class ByteSlicePool {
public:
    /// Slice sizes by level; the first slice is 5 bytes.
    static constexpr std::array<int32_t, 10> LEVEL_SIZE_ARRAY{5, 14, 20, 30, 40, 40, 80, 80, 120, 200};
    /// `NEXT_LEVEL_ARRAY[x] == x + 1`, except the last entry, which stays at the
    /// maximum slice size.
    static constexpr std::array<size_t, 10> NEXT_LEVEL_ARRAY{1, 2, 3, 4, 5, 6, 7, 8, 9, 9};
    /// Size of a freshly allocated slice.
    static constexpr int32_t FIRST_LEVEL_SIZE = LEVEL_SIZE_ARRAY[0];

    ByteSlicePool() = default;

    /// Allocates a new level-0 slice of `size` bytes and returns its offset.
    [[nodiscard]] Result<int32_t> new_slice(int32_t size, ByteBlockPool &pool);

    /// Continues `slice_index` (`upto` points at its last byte) with a slice of
    /// the next level, returning the offset of the new slice.
    [[nodiscard]] Result<size_t> alloc_slice(size_t slice_index, size_t upto, ByteBlockPool &pool);

    /// Like [`alloc_slice`], but returns `(offset << 8) | (new_size - 3)`.
    [[nodiscard]] Result<int32_t> alloc_known_size_slice(size_t slice_index, size_t upto, ByteBlockPool &pool);
};

} // namespace lucene
