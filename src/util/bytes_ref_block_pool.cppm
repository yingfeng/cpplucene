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

export module lucene_core:bytes_ref_block_pool;

import :types;
import std;

import :byte_block_pool;
import :bytes_ref;
import :error;

namespace lucene {

/// Where a term lives inside a [`ByteBlockPool`].
///
/// Port of `BytesRefBlockPoolPosition` (`util/bytes_ref_block_pool.rs`).  The
/// location stays valid only while the pool's contents are unchanged.
export struct BytesRefBlockPoolPosition {
    size_t block_index{0};
    size_t offset{0};
    size_t length{0};
};

/// Length-prefixed `BytesRef` storage on top of a [`ByteBlockPool`].
///
/// Port of `util/bytes_ref_block_pool.rs`.  A term is stored as a 1-byte length
/// (or a 2-byte big-endian length with the high bit set when it does not fit in
/// 7 bits) followed by the bytes.  A value never straddles a block boundary:
/// [`add_bytes_ref`] advances the pool to a fresh block when it would.
export class BytesRefBlockPool {
public:
    BytesRefBlockPool() = default;

    /// Returns the underlying pool to its empty state.
    void reset(ByteBlockPool &byte_block_pool);

    /// Resolves the term starting at `start` without copying its bytes.
    [[nodiscard]] BytesRefBlockPoolPosition fill_bytes_ref(int32_t start, const ByteBlockPool &byte_block_pool) const;

    /// Copies the bytes at `position` out of the pool.
    [[nodiscard]] Result<BytesRef> bytes_ref(const BytesRefBlockPoolPosition &position,
                                             const ByteBlockPool &byte_block_pool) const;

    /// Stores `bytes` and returns the start offset to read it back with
    /// [`fill_bytes_ref`].
    [[nodiscard]] Result<int32_t> add_bytes_ref(const BytesRef &bytes, ByteBlockPool &pool);

    /// Hash of the term at `start`, with the same value as
    /// `BytesRef::hash_code()` of that term.
    [[nodiscard]] int32_t hash(int32_t start, const ByteBlockPool &pool) const;

    /// `true` when the term at `start` equals `b`.
    [[nodiscard]] bool equals(int32_t start, const BytesRef &b, const ByteBlockPool &pool) const;
};

} // namespace lucene
