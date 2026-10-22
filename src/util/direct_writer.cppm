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

export module lucene_core:direct_writer;

import :types;
import :error;

import std;

import :data_output;

namespace lucene {

/// Writes packed integers so that [`DirectReader`] can read them back with a
/// single I/O per value.
///
/// Port of `util/packed/direct_writer.rs` (Lucene's `DirectWriter`).  Unlike
/// `PackedInts`, this optimizes for read I/O and supports value counts above
/// 2^31.
///
/// **On-disk layout (must stay byte-identical to Lucene):** values are packed
/// *little endian* and *LSB first*; the value at index `i` starts at bit
/// `i * bits_per_value` of the little-endian bit stream.  This is **not** the
/// `Format.PACKED` layout of `BulkOperationPacked`, which is a big-endian
/// MSB-first bit stream.
///
/// Only the bit widths in [`SUPPORTED_BITS_PER_VALUE`] are accepted; use
/// [`bits_required`] to round a required width up to a supported one.
export class DirectWriter {
public:
    /// Bit widths this format supports (all but 1 and 2 are multiples of 4).
    static constexpr std::array<int32_t, 14> SUPPORTED_BITS_PER_VALUE{1,  2,  4,  8,  12, 16, 20,
                                                                     24, 28, 32, 40, 48, 56, 64};

    /// Creates a writer that will write `num_values` values of
    /// `bits_per_value` bits each.  Validates the bit width like
    /// `DirectWriter.get_instance`.
    [[nodiscard]] static Result<DirectWriter> get_instance(DataOutput &output, int64_t num_values,
                                                           int32_t bits_per_value);

    [[nodiscard]] VoidResult add(int64_t value);

    /// Flushes the buffered values and writes the padding bytes that let the
    /// reader fetch the last values with a single word read.
    [[nodiscard]] VoidResult finish();

    [[nodiscard]] int32_t bits_per_value() const noexcept { return bits_per_value_; }
    [[nodiscard]] int64_t count() const noexcept { return count_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    /// Bits required for `max_value`, rounded up to a supported width.
    [[nodiscard]] static Result<int32_t> bits_required(int64_t max_value);

    /// `PackedInts::unsigned_bits_required` rounded up to a supported width.
    [[nodiscard]] static int32_t unsigned_bits_required(int64_t max_value);

private:
    DirectWriter(DataOutput &output, int64_t num_values, int32_t bits_per_value);

    [[nodiscard]] VoidResult flush();
    static void encode(std::span<const int64_t> values, size_t upto, std::span<uint8_t> blocks,
                       int32_t bits_per_value);

    DataOutput *output_;
    int32_t bits_per_value_;
    int64_t num_values_;
    int64_t count_{0};
    bool finished_{false};
    size_t off_{0};
    std::vector<uint8_t> next_blocks_;
    std::vector<int64_t> next_values_;
};

} // namespace lucene
