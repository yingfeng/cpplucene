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

module lucene_core:ut.pfor_util;

import lucene_core;

using namespace lucene;

namespace {

/// Encodes `values`, returns the bytes written.
std::vector<uint8_t> encode(PForUtil &pfor, std::span<int32_t> values) {
    std::vector<uint8_t> bytes(values.size() * 8 + 64);
    ByteArrayDataOutput out(bytes);
    auto result = pfor.encode(values, out);
    EXPECT_TRUE(result.has_value());
    bytes.resize(out.get_position());
    return bytes;
}

/// Decodes 128 values out of `bytes`.
std::vector<int32_t> decode(PForUtil &pfor, std::span<const uint8_t> bytes) {
    ByteArrayDataInput in(bytes);
    PostingDecodingUtil pdu(in);
    std::vector<int32_t> decoded(ForUtil::BLOCK_SIZE);
    auto result = pfor.decode(pdu, decoded);
    EXPECT_TRUE(result.has_value());
    return decoded;
}

} // namespace

TEST(PForUtilTest, RoundTracksExceptions) {
    // Mostly small values with a few spikes large enough to need patching.
    std::vector<int32_t> values(ForUtil::BLOCK_SIZE);
    for (size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<int32_t>((i * 97) & 0x3FFu);
    }
    values[7] = 1 << 20;
    values[64] = 1 << 22;
    values[127] = 1 << 24;

    PForUtil encoder;
    const std::vector<int32_t> expected = values;
    const std::vector<uint8_t> bytes = encode(encoder, values);

    PForUtil decoder;
    EXPECT_EQ(decode(decoder, bytes), expected);
    // 4 bytes of exceptions (2 per spike) on top of the patched block.
    EXPECT_GT(bytes.size(), static_cast<size_t>(ForUtil::num_bytes(13)));
}

TEST(PForUtilTest, AllEqualUsesTheVintShortcut) {
    std::vector<int32_t> values(ForUtil::BLOCK_SIZE, 42);

    PForUtil encoder;
    const std::vector<uint8_t> bytes = encode(encoder, values);
    // Token byte (no exceptions, 0 bits per value) + one vint.
    EXPECT_EQ(bytes.size(), 2u);
    EXPECT_EQ(bytes[0], 0u);

    PForUtil decoder;
    const std::vector<int32_t> decoded = decode(decoder, bytes);
    EXPECT_EQ(decoded, std::vector<int32_t>(ForUtil::BLOCK_SIZE, 42));
}

TEST(PForUtilTest, AllEqualWithPatchedExceptions) {
    std::vector<int32_t> values(ForUtil::BLOCK_SIZE, 1);
    values[3] = 1 << 12;
    values[100] = 1 << 10;

    PForUtil encoder;
    const std::vector<int32_t> expected = values;
    const std::vector<uint8_t> bytes = encode(encoder, values);

    PForUtil decoder;
    EXPECT_EQ(decode(decoder, bytes), expected);
}

TEST(PForUtilTest, RoundTripsZeroes) {
    std::vector<int32_t> values(ForUtil::BLOCK_SIZE, 0);

    PForUtil encoder;
    const std::vector<uint8_t> bytes = encode(encoder, values);

    PForUtil decoder;
    EXPECT_EQ(decode(decoder, bytes), values);
}

TEST(PForUtilTest, SkipMatchesDecodeLength) {
    std::vector<int32_t> values(ForUtil::BLOCK_SIZE);
    for (size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<int32_t>((i * 1237) & 0x3FFFFu);
    }
    values[5] = 1 << 21;
    const std::vector<int32_t> expected = values;

    PForUtil encoder;
    const std::vector<uint8_t> bytes = encode(encoder, values);

    // Bytes consumed by decode ...
    ByteArrayDataInput decodeInput(bytes);
    PostingDecodingUtil pdu(decodeInput);
    PForUtil decoder;
    std::vector<int32_t> decoded(ForUtil::BLOCK_SIZE);
    ASSERT_TRUE(decoder.decode(pdu, decoded).has_value());
    ASSERT_EQ(decoded, expected);
    const size_t decoded_length = decodeInput.get_position();

    // ... must match what skip() jumps over.
    ByteArrayDataInput skipInput(bytes);
    ASSERT_TRUE(PForUtil::skip(skipInput).has_value());
    EXPECT_EQ(skipInput.get_position(), decoded_length);
    EXPECT_EQ(skipInput.get_position(), bytes.size());
}

TEST(PForUtilTest, AllEqualSkipMatchesDecodeLength) {
    std::vector<int32_t> values(ForUtil::BLOCK_SIZE, 7);

    PForUtil encoder;
    const std::vector<uint8_t> bytes = encode(encoder, values);

    ByteArrayDataInput skipInput(bytes);
    ASSERT_TRUE(PForUtil::skip(skipInput).has_value());
    EXPECT_EQ(skipInput.get_position(), bytes.size());
}
