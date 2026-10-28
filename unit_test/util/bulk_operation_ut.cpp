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

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

module lucene_core:ut.bulk_operation;

import lucene_core;

using namespace lucene;

namespace {

/// The per-bit encoder, kept as the reference the bulk operation has to match.
std::vector<uint8_t> encode_bit_by_bit(const std::vector<int64_t> &values, int32_t bits_per_value) {
    const size_t byte_count = (values.size() * static_cast<size_t>(bits_per_value) + 7) / 8;
    std::vector<uint8_t> bytes(byte_count, 0);
    size_t bit_position = 0;
    for (const int64_t value : values) {
        const uint64_t bits = static_cast<uint64_t>(value);
        for (int32_t bit = bits_per_value - 1; bit >= 0; --bit, ++bit_position) {
            if (((bits >> bit) & 1U) != 0) {
                bytes[bit_position >> 3] |= static_cast<uint8_t>(0x80U >> (bit_position & 7));
            }
        }
    }
    return bytes;
}

std::vector<int64_t> decode_bit_by_bit(std::span<const uint8_t> bytes, size_t count, int32_t bits_per_value) {
    std::vector<int64_t> values(count, 0);
    size_t bit_position = 0;
    for (size_t i = 0; i < count; ++i) {
        uint64_t value = 0;
        for (int32_t bit = 0; bit < bits_per_value; ++bit, ++bit_position) {
            const uint8_t byte = bit_position >> 3 < bytes.size() ? bytes[bit_position >> 3] : 0;
            value = (value << 1) | static_cast<uint64_t>((byte >> (7 - (bit_position & 7))) & 1U);
        }
        values[i] = static_cast<int64_t>(value);
    }
    return values;
}

std::mt19937_64 &rng() {
    static std::mt19937_64 engine(42);
    return engine;
}

int64_t random_value(int32_t bits_per_value) {
    if (bits_per_value >= 64) {
        return static_cast<int64_t>(rng()());
    }
    const uint64_t mask = (uint64_t{1} << bits_per_value) - 1;
    return static_cast<int64_t>(rng()() & mask);
}

} // namespace

TEST(BulkOperationTest, EveryWidthMatchesTheBitByBitLayout) {
    for (int32_t bits = 1; bits <= 64; ++bits) {
        SCOPED_TRACE("bits=" + std::to_string(bits));
        const BulkOperation &operation = bulk_operation(bits);
        EXPECT_EQ(operation.bits_per_value(), bits);

        std::vector<int64_t> values(37);
        for (int64_t &value : values) {
            value = random_value(bits);
        }
        // A width that is not a divisor of eight makes values straddle bytes, so
        // the check covers the window arithmetic.
        std::vector<uint8_t> blocks(operation.byte_count(values.size()), 0);
        ASSERT_EQ(blocks.size(), encode_bit_by_bit(values, bits).size());

        operation.encode(std::span<int64_t>(values), 0, std::span<uint8_t>(blocks));
        EXPECT_EQ(blocks, encode_bit_by_bit(values, bits)) << "the encoding must be identical";

        std::vector<int64_t> decoded(values.size(), -1);
        operation.decode(std::span<const uint8_t>(blocks), 0, std::span<int64_t>(decoded));
        EXPECT_EQ(decoded, values);
        EXPECT_EQ(decoded, decode_bit_by_bit(std::span<const uint8_t>(blocks), values.size(), bits));
    }
}

TEST(BulkOperationTest, ExtremeValuesRoundTrip) {
    for (int32_t bits : {1, 7, 13, 31, 32, 33, 55, 56, 57, 63, 64}) {
        SCOPED_TRACE("bits=" + std::to_string(bits));
        const BulkOperation &operation = bulk_operation(bits);
        const int64_t max = bits >= 64 ? -1 : static_cast<int64_t>((uint64_t{1} << bits) - 1);
        std::vector<int64_t> values{0, max, max, 0, max / 2, 1};
        std::vector<uint8_t> blocks(operation.byte_count(values.size()), 0);
        operation.encode(std::span<int64_t>(values), 0, std::span<uint8_t>(blocks));
        std::vector<int64_t> decoded(values.size(), -1);
        operation.decode(std::span<const uint8_t>(blocks), 0, std::span<int64_t>(decoded));
        EXPECT_EQ(decoded, values);
    }
}

TEST(BulkOperationTest, DecodesASliceAndKeepsTheBytesAroundAnEncodedSlice) {
    const BulkOperation &operation = bulk_operation(13);
    std::vector<int64_t> values(20);
    for (int64_t &value : values) {
        value = random_value(13);
    }
    std::vector<uint8_t> blocks(operation.byte_count(values.size()), 0);
    operation.encode(std::span<int64_t>(values), 0, std::span<uint8_t>(blocks));

    // Decoding from the middle of the stream has to give the same values.
    std::vector<int64_t> tail(5, -1);
    operation.decode(std::span<const uint8_t>(blocks), 15, std::span<int64_t>(tail));
    for (size_t i = 0; i < tail.size(); ++i) {
        EXPECT_EQ(tail[i], values[15 + i]) << i;
    }

    // Encoding into a slice leaves the bits of the other values alone.
    std::vector<uint8_t> patched = blocks;
    std::vector<int64_t> replacement{1, 2, 3};
    operation.encode(std::span<int64_t>(replacement), 4, std::span<uint8_t>(patched));
    std::vector<int64_t> all(values.size(), 0);
    operation.decode(std::span<const uint8_t>(patched), 0, std::span<int64_t>(all));
    for (size_t i = 0; i < all.size(); ++i) {
        const int64_t expected = (i >= 4 && i < 7) ? replacement[i - 4] : values[i];
        EXPECT_EQ(all[i], expected) << i;
    }
}

TEST(BulkOperationTest, DecodeToleratesATruncatedBlock) {
    const BulkOperation &operation = bulk_operation(23);
    std::vector<int64_t> values{7, 1000, 4000000};
    const std::vector<uint8_t> full = encode_bit_by_bit(values, 23);
    // The last window would need a byte the caller does not own; it reads as zero
    // rather than running off the end.
    std::vector<uint8_t> truncated(full.begin(), full.end() - 1);
    std::vector<int64_t> decoded(values.size(), -1);
    operation.decode(std::span<const uint8_t>(truncated), 0, std::span<int64_t>(decoded));
    EXPECT_EQ(decoded[0], values[0]);
    EXPECT_EQ(decoded[1], values[1]);
    // The third value lost its low bits that lived in the missing byte.
    EXPECT_EQ(decoded[2] >> 8, values[2] >> 8);
}

TEST(BulkOperationTest, RejectsAnImpossibleWidth) {
    EXPECT_THROW(static_cast<void>(bulk_operation(0)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(bulk_operation(65)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(bulk_operation(-1)), std::invalid_argument);
}

TEST(BulkOperationTest, ThePackedStreamUsesTheSameLayout) {
    // `write_packed_stream` / `read_packed_stream` go through the bulk operation;
    // this pins the layout both of them share.
    ByteBuffersDirectory directory;
    const int32_t bits = 19;
    std::vector<int64_t> values(29);
    for (int64_t &value : values) {
        value = random_value(bits);
    }
    auto output = directory.create_output("packed.bin", io_context_default());
    ASSERT_TRUE(output.has_value());
    ASSERT_TRUE(write_packed_stream(*output.value(), std::span<const int64_t>(values), bits).has_value());
    ASSERT_TRUE(output.value()->close().has_value());

    auto input = directory.open_input("packed.bin", io_context_default());
    ASSERT_TRUE(input.has_value());
    auto decoded = read_packed_stream(*input.value(), values.size(), bits);
    ASSERT_TRUE(decoded.has_value()) << (decoded ? "" : decoded.error().to_string());
    EXPECT_EQ(decoded.value(), values);
    EXPECT_EQ(encode_bit_by_bit(values, bits),
              encode_bit_by_bit({decoded.value().begin(), decoded.value().end()}, bits));
}
