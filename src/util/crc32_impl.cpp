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

module lucene_core:crc32.impl;

import :crc32;
import :types;

import std;

namespace lucene {

namespace {

/// Slice-by-8 tables, generated once at first use.
struct Crc32Tables {
    std::array<std::array<uint32_t, 256>, 8> values{};

    Crc32Tables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1u) != 0 ? (crc >> 1) ^ Crc32::POLYNOMIAL : crc >> 1;
            }
            values[0][i] = crc;
        }
        for (uint32_t i = 0; i < 256; ++i) {
            for (size_t table = 1; table < 8; ++table) {
                const uint32_t previous = values[table - 1][i];
                values[table][i] = (previous >> 8) ^ values[0][previous & 0xFFu];
            }
        }
    }
};

const Crc32Tables &tables() {
    static const Crc32Tables instance;
    return instance;
}

} // namespace

void Crc32::update(uint8_t byte) { crc_ = (crc_ >> 8) ^ tables().values[0][(crc_ ^ byte) & 0xFFu]; }

void Crc32::update(std::span<const uint8_t> bytes) {
    const auto &t = tables().values;
    uint32_t crc = crc_;
    size_t i = 0;
    // Slice-by-8 fast path.
    for (; i + 8 <= bytes.size(); i += 8) {
        const uint64_t block = static_cast<uint64_t>(bytes[i]) | (static_cast<uint64_t>(bytes[i + 1]) << 8) |
                               (static_cast<uint64_t>(bytes[i + 2]) << 16) |
                               (static_cast<uint64_t>(bytes[i + 3]) << 24) |
                               (static_cast<uint64_t>(bytes[i + 4]) << 32) |
                               (static_cast<uint64_t>(bytes[i + 5]) << 40) |
                               (static_cast<uint64_t>(bytes[i + 6]) << 48) |
                               (static_cast<uint64_t>(bytes[i + 7]) << 56);
        const uint64_t mixed = block ^ crc;
        crc = t[7][(mixed) & 0xFFu] ^ t[6][(mixed >> 8) & 0xFFu] ^ t[5][(mixed >> 16) & 0xFFu] ^
              t[4][(mixed >> 24) & 0xFFu] ^ t[3][(mixed >> 32) & 0xFFu] ^ t[2][(mixed >> 40) & 0xFFu] ^
              t[1][(mixed >> 48) & 0xFFu] ^ t[0][(mixed >> 56) & 0xFFu];
    }
    for (; i < bytes.size(); ++i) {
        crc = (crc >> 8) ^ t[0][(crc ^ bytes[i]) & 0xFFu];
    }
    crc_ = crc;
}

uint64_t Crc32::get_value() const { return static_cast<uint64_t>(crc_ ^ 0xFFFFFFFFu); }

} // namespace lucene
