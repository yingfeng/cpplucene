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

export module lucene_core:block_packed_reader_iterator;

import :types;
import std;

import :data_input;
import :error;

namespace lucene {

/// Reads a sequence written by [`BlockPackedWriter`].
///
/// Port of `BlockPackedReaderIterator`
/// (`core/util/packed/block_packed_reader_iterator.rs`).  The reader keeps one
/// decoded block in memory; [`reset`] starts a new sequence in the same
/// input, which is how the term-vectors reader walks the several packed
/// sequences of a chunk one after the other.
export class BlockPackedReaderIterator {
public:
    /// Creates a reader for at most `value_count` values of `block_size`
    /// (a multiple of 64).
    [[nodiscard]] static Result<BlockPackedReaderIterator> create(int32_t packed_ints_version,
                                                                 size_t block_size, size_t value_count);

    /// Restarts at the current position of `in` with a new value count; the
    /// decoded block is dropped, the next call refills it.
    void reset(size_t value_count) noexcept;

    /// Skips exactly `count` values.
    [[nodiscard]] VoidResult skip(size_t count, DataInput &in);

    /// The next value.
    [[nodiscard]] Result<int64_t> next_value(DataInput &in);

    /// Reads between 1 and `count` values.  The returned span points into the
    /// internal block buffer, so it stays valid only until the next call.
    [[nodiscard]] Result<std::span<const int64_t>> next_batch(size_t count, DataInput &in);

    /// Number of values consumed since the last [`reset`].
    [[nodiscard]] size_t ord() const noexcept { return ord_; }

private:
    BlockPackedReaderIterator(int32_t packed_ints_version, size_t block_size);

    /// Decodes the next block from `in`.
    [[nodiscard]] VoidResult refill(DataInput &in);

    /// `DataOutput::read_vlong` that also yields negative values.
    [[nodiscard]] static Result<int64_t> read_vlong(DataInput &in);

    std::vector<int64_t> values_;
    size_t block_size_{0};
    size_t value_count_{0};
    /// Index of the next buffered value; equals `block_size_` when the buffer is
    /// exhausted.
    size_t off_{0};
    size_t ord_{0};
    int32_t packed_ints_version_{0};
};

} // namespace lucene
