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

export module lucene_core:crc32;

import :types;

import std;

namespace lucene {

/// CRC-32 (ISO-HDLC, the `java.util.zip.CRC32` polynomial), used by Lucene for
/// the compound-file checksums.  Port of `core/util/crc32.rs`.
export class Crc32 {
public:
    Crc32() = default;

    void update(std::span<const uint8_t> bytes);
    void update(uint8_t byte);

    /// The current checksum value.
    [[nodiscard]] uint64_t get_value() const;

    void reset() noexcept { crc_ = 0xFFFFFFFFu; }

    static constexpr uint32_t POLYNOMIAL = 0xEDB88320u;

private:
    uint32_t crc_{0xFFFFFFFFu};
};

} // namespace lucene
