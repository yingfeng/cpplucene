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

module lucene_core:ut.for_delta_util;

import lucene_core;

using namespace lucene;

namespace {

/// Encodes `deltas` and returns the bytes written.
std::vector<uint8_t> encode(ForDeltaUtil &util, std::span<int32_t> deltas) {
    std::vector<uint8_t> bytes(deltas.size() * 8 + 64);
    ByteArrayDataOutput out(bytes);
    auto result = util.encode_deltas(deltas, out);
    EXPECT_TRUE(result.has_value());
    bytes.resize(out.get_position());
    return bytes;
}

/// Decodes a block and returns the 128 absolute values.
std::vector<int32_t> decode(ForDeltaUtil &util, std::span<const uint8_t> bytes, int32_t base) {
    ByteArrayDataInput in(bytes);
    PostingDecodingUtil pdu(in);
    std::vector<int32_t> ints(ForDeltaUtil::BLOCK_SIZE);
    auto result = util.decode_and_prefix_sum(pdu, base, ints);
    EXPECT_TRUE(result.has_value());
    return ints;
}

/// Expected absolute values: `base` plus the running sum of the deltas.
std::vector<int32_t> expected_values(std::span<const int32_t> deltas, int32_t base) {
    std::vector<int32_t> values(deltas.size());
    int32_t carry = base;
    for (size_t i = 0; i < deltas.size(); ++i) {
        carry = static_cast<int32_t>(static_cast<uint32_t>(carry) + static_cast<uint32_t>(deltas[i]));
        values[i] = carry;
    }
    return values;
}

} // namespace

TEST(ForDeltaUtilTest, AllOnesUsesTheSingleByteShortcut) {
    std::vector<int32_t> deltas(ForDeltaUtil::BLOCK_SIZE, 1);

    ForDeltaUtil encoder;
    const std::vector<uint8_t> bytes = encode(encoder, deltas);
    EXPECT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes[0], 0u);

    ForDeltaUtil decoder;
    const std::vector<int32_t> decoded = decode(decoder, bytes, 5);
    ASSERT_EQ(decoded.size(), ForDeltaUtil::BLOCK_SIZE);
    EXPECT_EQ(decoded.front(), 6);
    EXPECT_EQ(decoded.back(), 6 + static_cast<int32_t>(ForDeltaUtil::BLOCK_SIZE) - 1);
}

TEST(ForDeltaUtilTest, RoundTripsEveryBitWidth) {
    ForDeltaUtil encoder;
    ForDeltaUtil decoder;

    for (int32_t bits_per_value = 2; bits_per_value <= 31; ++bits_per_value) {
        std::vector<int32_t> deltas(ForDeltaUtil::BLOCK_SIZE, 1);
        deltas[0] = int32_t{1} << (bits_per_value - 1);

        const std::vector<int32_t> expected = expected_values(deltas, 17);
        const std::vector<uint8_t> bytes = encode(encoder, deltas);
        ASSERT_EQ(bytes.size(), 1u + static_cast<size_t>(ForUtil::num_bytes(bits_per_value)))
                << "bits_per_value=" << bits_per_value;
        ASSERT_EQ(bytes[0], static_cast<uint8_t>(bits_per_value)) << "bits_per_value=" << bits_per_value;

        const std::vector<int32_t> decoded = decode(decoder, bytes, 17);
        EXPECT_EQ(decoded, expected) << "bits_per_value=" << bits_per_value;
    }
}

TEST(ForDeltaUtilTest, RoundTripsNarrowLaneWidths) {
    ForDeltaUtil encoder;
    ForDeltaUtil decoder;

    // Up to 3 bits: four 8-bit lanes per integer.
    for (int32_t bits_per_value = 1; bits_per_value <= 3; ++bits_per_value) {
        std::vector<int32_t> deltas(ForDeltaUtil::BLOCK_SIZE);
        for (size_t i = 0; i < deltas.size(); ++i) {
            deltas[i] = 1 + static_cast<int32_t>(i % ((1u << bits_per_value) - 1u));
        }
        // Force the width by putting a value of exactly `bits_per_value` bits
        // first (except for the all-ones shortcut).
        deltas[0] = int32_t{1} << (bits_per_value - 1);

        const std::vector<int32_t> expected = expected_values(deltas, 1);
        const std::vector<uint8_t> bytes = encode(encoder, deltas);
        EXPECT_EQ(decode(decoder, bytes, 1), expected) << "bits_per_value=" << bits_per_value;
    }

    // Up to 10 bits: two 16-bit lanes per integer.
    for (int32_t bits_per_value = 4; bits_per_value <= 10; ++bits_per_value) {
        std::vector<int32_t> deltas(ForDeltaUtil::BLOCK_SIZE, 1);
        deltas[0] = int32_t{1} << (bits_per_value - 1);
        deltas[64] = int32_t{1} << (bits_per_value - 2);

        const std::vector<int32_t> expected = expected_values(deltas, 100);
        const std::vector<uint8_t> bytes = encode(encoder, deltas);
        EXPECT_EQ(decode(decoder, bytes, 100), expected) << "bits_per_value=" << bits_per_value;
    }
}

TEST(ForDeltaUtilTest, IncreasingSequenceWithRealisticGaps) {
    std::vector<int32_t> deltas(ForDeltaUtil::BLOCK_SIZE);
    for (size_t i = 0; i < deltas.size(); ++i) {
        deltas[i] = static_cast<int32_t>(1 + (i * 37) % 1000);
    }
    const int32_t base = 65536;
    const std::vector<int32_t> expected = expected_values(deltas, base);

    ForDeltaUtil encoder;
    const std::vector<uint8_t> bytes = encode(encoder, deltas);

    ForDeltaUtil decoder;
    EXPECT_EQ(decode(decoder, bytes, base), expected);
}

TEST(ForDeltaUtilTest, PrefixSum32MatchesRunningSum) {
    std::vector<int32_t> values(ForDeltaUtil::BLOCK_SIZE);
    for (size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<int32_t>((i * 991) & 0xFFFF);
    }
    const std::vector<int32_t> expected = expected_values(values, 7);

    ForDeltaUtil::prefix_sum32(values, 7);
    EXPECT_EQ(values, expected);
}

TEST(ForDeltaUtilTest, PrefixSumOfOnesIgnoresInput) {
    std::vector<int32_t> values(ForDeltaUtil::BLOCK_SIZE, 1234);
    ForDeltaUtil::prefix_sum_of_ones(values, 10);
    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(values[i], 10 + static_cast<int32_t>(i) + 1) << "i=" << i;
    }
}
