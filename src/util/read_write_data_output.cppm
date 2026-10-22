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

export module lucene_core:read_write_data_output;

import :types;
import :error;

import std;

import :data_output;
import :reverse_bytes_reader;

namespace lucene {

/// A write buffer that later becomes a readable byte store.
///
/// Port of `util/fst_impl/read_write_data_output.rs`.  The FST compiler writes a
/// node **backwards** into this buffer, then either hands a
/// [`ReverseBytesReader`] to the FST (in-memory), or copies the bytes forward
/// into the real index file.
///
/// **Deviation:** rlucene wraps `ByteBuffersDataOutput` and chooses between a
/// paged reader and a contiguous reader depending on `block_bits`.  This port
/// keeps a single contiguous `std::vector`, which is simpler and has the same
/// observable behaviour for the FST use case; the `block_bits` parameter is
/// accepted and ignored so ported call sites stay unchanged.
export class ReadWriteDataOutput final : public DataOutput {
public:
    explicit ReadWriteDataOutput(int32_t block_bits = 15, bool contiguous_reader = true);

    /// Keeps the base class' one-argument convenience overload visible.
    using DataOutput::write_bytes;

    [[nodiscard]] VoidResult write_byte(uint8_t b) override;
    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override;

    /// Marks the buffer complete; required before it can be read.
    [[nodiscard]] VoidResult freeze();

    /// `true` once [`freeze`] was called.
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

    /// A reader that walks the bytes backwards (the FST direction).
    ///
    /// **The caller must keep this output alive** for as long as the reader is
    /// used; the reader borrows the internal buffer.
    [[nodiscard]] Result<ReverseBytesReader> get_reverse_bytes_reader() const;

    /// Copies the buffer forward into `out`.
    [[nodiscard]] VoidResult write_to(DataOutput &out) const;

    [[nodiscard]] size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] std::span<const uint8_t> bytes() const noexcept { return bytes_; }
    [[nodiscard]] int64_t ram_bytes_used() const noexcept { return static_cast<int64_t>(bytes_.size()); }
    void reset() noexcept;

    [[nodiscard]] std::string to_string() const override { return "ReadWriteDataOutput"; }

private:
    std::vector<uint8_t> bytes_;
    bool frozen_{false};
};

} // namespace lucene
