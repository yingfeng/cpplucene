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

module lucene_core:checksum.impl;

import :checksum;
import :debug;
import :types;

import std;

namespace lucene {

namespace {

constexpr uint32_t CRC32_POLYNOMIAL = 0xEDB88320U;

/// The reflected CRC32 lookup table, built once on first use.
const std::array<uint32_t, 256> &crc32_table() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> entries{};
        for (uint32_t i = 0; i < entries.size(); ++i) {
            uint32_t value = i;
            for (int32_t bit = 0; bit < 8; ++bit) {
                value = (value & 1U) != 0 ? (value >> 1) ^ CRC32_POLYNOMIAL : value >> 1;
            }
            entries[i] = value;
        }
        return entries;
    }();
    return table;
}

} // namespace

void Crc32Checksum::update(uint8_t b) {
    state_ = crc32_table()[(state_ ^ b) & 0xFFU] ^ (state_ >> 8);
}

void Crc32Checksum::update_bytes(std::span<const uint8_t> bytes, size_t offset, size_t len) {
    debug_check(offset + len <= bytes.size());
    const auto &table = crc32_table();
    uint32_t state = state_;
    for (size_t i = offset; i < offset + len; ++i) {
        state = table[(state ^ bytes[i]) & 0xFFU] ^ (state >> 8);
    }
    state_ = state;
}

int64_t Crc32Checksum::get_value() {
    return static_cast<int64_t>(state_ ^ 0xFFFFFFFFU);
}

void Crc32Checksum::reset() {
    state_ = 0xFFFFFFFFU;
}

} // namespace lucene
