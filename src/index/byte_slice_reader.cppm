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

export module lucene_core:byte_slice_reader;

import :types;
import std;

import :byte_block_pool;
import :data_input;
import :data_output;
import :error;

namespace lucene {

/// A [`DataInput`] that reads the chain of byte slices written by
/// [`ByteSlicePool`](:byte_slice_pool).
///
/// Port of `index/byte_slice_reader.rs`.  Each slice is read to its end, then
/// the 4-byte forwarding address stored there is followed to the next slice;
/// the slice level (and therefore its size) is recovered from the marker byte
/// written by the slice pool.
///
/// The pool is referenced, not owned: the caller must keep it alive (and
/// unchanged) while the reader is in use.
export class ByteSliceReader final : public DataInput {
public:
    explicit ByteSliceReader(const ByteBlockPool &pool) : pool_(pool) {}

    ~ByteSliceReader() override = default;

    /// Positions the reader at the slice chain covering `[start_index, end_index)`.
    void init(size_t start_index, size_t end_index);

    /// `true` once the whole chain has been read.
    [[nodiscard]] bool eof() const;

    /// Moves to the next slice in the chain.
    [[nodiscard]] VoidResult next_slice();

    /// Copies the rest of the chain into `out`; returns the number of bytes.
    [[nodiscard]] size_t write(DataOutput &out);

    // ---- DataInput ----

    [[nodiscard]] Result<uint8_t> read_byte() override;
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override;
    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override;
    [[nodiscard]] std::string to_string() const override { return "ByteSliceReader"; }

private:
    const ByteBlockPool &pool_;
    size_t buffer_upto_{0};
    size_t upto_{0};
    size_t limit_{0};
    size_t level_{0};
    size_t buffer_offset_{0};
    size_t end_index_{0};
};

} // namespace lucene
