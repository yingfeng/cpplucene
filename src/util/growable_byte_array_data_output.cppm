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

export module lucene_core:growable_byte_array_data_output;

import :types;
import :error;

import std;

import :data_output;

namespace lucene {

/// A [`DataOutput`] over a growable byte array.
///
/// Port of `util/fst_impl/growable_byte_array_data_output.rs`.  Used where the
/// serialized size is not known up front, e.g. the empty-string output of an
/// FST's metadata.
export class GrowableByteArrayDataOutput final : public DataOutput {
public:
    GrowableByteArrayDataOutput() = default;
    explicit GrowableByteArrayDataOutput(size_t initial_capacity);

    [[nodiscard]] VoidResult write_byte(uint8_t b) override;
    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override;

    [[nodiscard]] size_t length() const noexcept { return bytes_.size(); }
    /// rlucene's `ByteBuffersDataOutput::size`; same as [`length`].
    [[nodiscard]] size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] const std::vector<uint8_t> &bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::vector<uint8_t> take_bytes() noexcept { return std::move(bytes_); }

    /// Appends the buffered bytes to `out`.
    ///
    /// This is `ByteBuffersDataOutput::copy_to` of rlucene, which the Lucene101
    /// postings writer uses to prepend length prefixes to an already encoded
    /// block.
    [[nodiscard]] VoidResult copy_to(DataOutput &out) const {
        return out.write_bytes(bytes_, 0, bytes_.size());
    }

    void reset() noexcept { bytes_.clear(); }
    [[nodiscard]] std::string to_string() const override { return "GrowableByteArrayDataOutput"; }

private:
    std::vector<uint8_t> bytes_;
};

} // namespace lucene
