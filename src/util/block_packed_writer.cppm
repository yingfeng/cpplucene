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

export module lucene_core:block_packed_writer;

import :types;
import std;

import :data_output;
import :error;

namespace lucene {

/// Writes blocks of non-decreasing (or arbitrary) `int64_t` values, encoding
/// each block with the fewest bits its own `[min, max]` range allows.
///
/// Port of `AbstractBlockPackedWriter` + `BlockPackedWriter`
/// (`core/util/packed/abstract_block_packed_writer.rs`, `block_packed_writer.rs`).
///
/// **On-disk format** (`<Block>^BlockCount`, `BlockCount = ceil(n / block_size)`):
///
/// ```text
/// Block  := <Token> [<MinValue>] [<PackedValues>]
/// Token  := one byte: bits 1..7 = bits per value, bit 0 = "min value is 0"
/// MinValue := zig-zag vlong of (min_adjusted - 1), only when bit 0 is clear
/// PackedValues := block_size values of `bits per value` bits, `Format.PACKED`
///                 layout (big endian, MSB first); absent when bits == 0
/// ```
///
/// A block whose values are all equal needs one byte (bits `0`, min `0`) or the
/// `MinValue` only.
export class BlockPackedWriter {
public:
    /// Smallest accepted block size.
    static constexpr int32_t MIN_BLOCK_SIZE = 64;
    /// Largest accepted block size (`1 << 27`).
    static constexpr int32_t MAX_BLOCK_SIZE = 1 << (30 - 3);
    /// Token bit that says the block minimum is `0`.
    static constexpr int32_t MIN_VALUE_EQUALS_0 = 1 << 0;
    /// Shift that turns the token into the number of bits per value.
    static constexpr int32_t BPV_SHIFT = 1;

    /// Creates a writer whose blocks hold `block_size` values; `block_size` must
    /// be a multiple of 64.
    [[nodiscard]] static Result<BlockPackedWriter> create(size_t block_size);

    /// Forgets the buffered values; the writer becomes usable again.
    void reset() noexcept;

    /// Appends one value, flushing the previous block when it is full.
    [[nodiscard]] VoidResult add(int64_t value, DataOutput &out);

    /// Flushes the partially filled block; no value may be added afterwards.
    [[nodiscard]] VoidResult finish(DataOutput &out);

    /// Number of values added since the last [`reset`].
    [[nodiscard]] size_t ord() const noexcept { return ord_; }

    /// Block size this writer was created with.
    [[nodiscard]] size_t block_size() const noexcept { return values_.size(); }

private:
    explicit BlockPackedWriter(size_t block_size);

    /// Encodes and writes the buffered block.
    [[nodiscard]] VoidResult flush(DataOutput &out);

    /// `DataOutput::write_vlong` that also accepts negative values.
    [[nodiscard]] static VoidResult write_vlong(DataOutput &out, int64_t value);

    std::vector<int64_t> values_;
    std::vector<int64_t> encoded_;
    std::vector<uint8_t> blocks_;
    size_t off_{0};
    size_t ord_{0};
    bool finished_{false};
};

} // namespace lucene
