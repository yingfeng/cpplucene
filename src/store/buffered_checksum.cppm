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

export module lucene_core:buffered_checksum;

import :types;
import std;

import :checksum;

namespace lucene {

/// Wraps another [`Checksum`] with an internal buffer so that many small
/// updates do not each cost a call into the digest.
///
/// Port of `BufferedChecksum` (`core/store/buffered_checksum.rs`).
export class BufferedChecksum : public Checksum {
public:
    /// Default buffer size: 1024 bytes.
    static constexpr size_t DEFAULT_BUFFER_SIZE = 1024;

    /// Takes ownership of `checksum` and uses [`DEFAULT_BUFFER_SIZE`].
    explicit BufferedChecksum(std::unique_ptr<Checksum> checksum);

    /// Takes ownership of `checksum` and buffers `buffer_size` bytes.
    BufferedChecksum(std::unique_ptr<Checksum> checksum, size_t buffer_size);

    void update(uint8_t b) override;
    void update_bytes(std::span<const uint8_t> bytes, size_t offset, size_t len) override;
    [[nodiscard]] int64_t get_value() override;
    void reset() override;

    // The primitives below mirror rlucene's `to_le_bytes`: the checksum of a
    // read primitive is the checksum of its little-endian bytes, which is what
    // `BufferedChecksumIndexInput` has to reproduce.

    void update_short(int16_t value);
    void update_int(int32_t value);
    void update_long(int64_t value);
    void update_longs(std::span<const int64_t> values, size_t offset, size_t len);

private:
    void flush();

    std::vector<uint8_t> buffer_;
    size_t upto_{0};
    std::unique_ptr<Checksum> checksum_;
};

} // namespace lucene
