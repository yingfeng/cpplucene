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

export module lucene_core:checksum;

import :types;
import std;

namespace lucene {

/// A checksum of a byte stream.
///
/// Port of the `Checksum` trait (`core/store/checksum.rs`).
export class Checksum {
public:
    virtual ~Checksum() = default;

    /// Adds one byte.
    virtual void update(uint8_t b) = 0;

    /// Adds `bytes[offset .. offset + len)`.
    virtual void update_bytes(std::span<const uint8_t> bytes, size_t offset, size_t len) = 0;

    /// The checksum of everything added so far.
    [[nodiscard]] virtual int64_t get_value() = 0;

    /// Returns to the state of a freshly constructed checksum.
    virtual void reset() = 0;
};

/// [`Checksum`] backed by CRC32 (`crc32fast`, i.e. the zlib/`CRC-32/ISO-HDLC`
/// variant: reflected polynomial `0xEDB88320`, initial value `0xFFFFFFFF`,
/// final XOR `0xFFFFFFFF`).
export class Crc32Checksum : public Checksum {
public:
    Crc32Checksum() = default;

    void update(uint8_t b) override;
    void update_bytes(std::span<const uint8_t> bytes, size_t offset, size_t len) override;
    [[nodiscard]] int64_t get_value() override;
    void reset() override;

private:
    uint32_t state_{0xFFFFFFFFU};
};

} // namespace lucene
