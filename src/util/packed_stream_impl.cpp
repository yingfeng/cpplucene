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

module lucene_core:packed_stream.impl;

import :packed_stream;

import :data_input;
import :data_output;
import :error;
import :types;

import std;

namespace lucene {

size_t packed_stream_byte_count(size_t value_count, int32_t bits_per_value) noexcept {
    if (bits_per_value <= 0) {
        return 0;
    }
    const size_t bits = value_count * static_cast<size_t>(bits_per_value);
    return (bits + 7) / 8;
}

VoidResult write_packed_stream(DataOutput &out, std::span<const int64_t> values, int32_t bits_per_value) {
    if (bits_per_value < 0 || bits_per_value > 64) {
        return err(LuceneError::illegal_argument("bits_per_value must be in [0, 64] (got " +
                                                 std::to_string(bits_per_value) + ")"));
    }
    const size_t byte_count = packed_stream_byte_count(values.size(), bits_per_value);
    if (byte_count == 0) {
        return {};
    }
    std::vector<uint8_t> bytes(byte_count, 0);
    size_t bit_position = 0;
    for (const int64_t value : values) {
        const uint64_t bits = static_cast<uint64_t>(value);
        for (int32_t bit = bits_per_value - 1; bit >= 0; --bit) {
            if (((bits >> bit) & 1U) != 0) {
                bytes[bit_position >> 3] |= static_cast<uint8_t>(0x80U >> (bit_position & 7));
            }
            ++bit_position;
        }
    }
    return out.write_bytes(std::span<const uint8_t>(bytes));
}

Result<std::vector<int64_t>> read_packed_stream(DataInput &in, size_t value_count, int32_t bits_per_value) {
    if (bits_per_value < 0 || bits_per_value > 64) {
        return err(LuceneError::illegal_argument("bits_per_value must be in [0, 64] (got " +
                                                 std::to_string(bits_per_value) + ")"));
    }
    std::vector<int64_t> values(value_count, 0);
    const size_t byte_count = packed_stream_byte_count(value_count, bits_per_value);
    if (byte_count == 0) {
        return values;
    }
    auto bytes = in.get_bytes(byte_count);
    if (!bytes.has_value()) {
        return std::unexpected(std::move(bytes.error()));
    }
    size_t bit_position = 0;
    for (size_t i = 0; i < value_count; ++i) {
        uint64_t value = 0;
        for (int32_t bit = 0; bit < bits_per_value; ++bit, ++bit_position) {
            const uint8_t byte = (*bytes)[bit_position >> 3];
            value = (value << 1) | static_cast<uint64_t>((byte >> (7 - (bit_position & 7))) & 1U);
        }
        values[i] = static_cast<int64_t>(value);
    }
    return values;
}

Result<int64_t> direct_packed_get(std::span<const uint8_t> bytes, int32_t bits_per_value, size_t index) {
    if (bits_per_value < 1 || bits_per_value > 64) {
        return err(LuceneError::illegal_argument("bits_per_value must be in [1, 64] (got " +
                                                 std::to_string(bits_per_value) + ")"));
    }
    const size_t bpv = static_cast<size_t>(bits_per_value);
    const size_t start_bit = index * bpv;
    const size_t first_byte = start_bit >> 3;
    const size_t last_byte = (start_bit + bpv - 1) >> 3;
    const size_t num_bytes = last_byte - first_byte + 1;
    if (last_byte >= bytes.size()) {
        return err(LuceneError::eof("packed value " + std::to_string(index) + " is outside the buffer"));
    }
    const size_t shift = start_bit & 7;
    uint64_t word = 0;
    for (size_t i = 0; i < num_bytes; ++i) {
        word |= static_cast<uint64_t>(bytes[first_byte + i]) << (8 * i);
    }
    const uint64_t mask =
            bits_per_value >= 64 ? std::numeric_limits<uint64_t>::max() : ((uint64_t{1} << bpv) - 1);
    return static_cast<int64_t>((word >> shift) & mask);
}

} // namespace lucene
