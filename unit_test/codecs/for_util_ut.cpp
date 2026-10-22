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

module lucene_core:ut.for_util;

import lucene_core;

using namespace lucene;

namespace {

constexpr uint32_t kGoldenRatio = 2654435761u;

/// Deterministic pseudo-random values that fit in `bits_per_value` bits.
std::vector<int32_t> make_values(int32_t bits_per_value) {
    const uint32_t mask = static_cast<uint32_t>(ForUtil::mask32(bits_per_value));
    std::vector<int32_t> values(ForUtil::BLOCK_SIZE);
    for (size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<int32_t>((static_cast<uint32_t>(i) * kGoldenRatio) & mask);
    }
    return values;
}

} // namespace

TEST(ForUtilTest, NumBytesIsSixteenBytesPerBit) {
    EXPECT_EQ(ForUtil::num_bytes(1), 16);
    EXPECT_EQ(ForUtil::num_bytes(8), 128);
    EXPECT_EQ(ForUtil::num_bytes(32), 512);
}

TEST(ForUtilTest, Masks) {
    EXPECT_EQ(ForUtil::mask32(0), 0);
    EXPECT_EQ(ForUtil::mask32(1), 1);
    EXPECT_EQ(ForUtil::mask32(8), 0xFF);
    EXPECT_EQ(ForUtil::mask32(31), 0x7FFFFFFF);
    EXPECT_EQ(ForUtil::mask32(32), -1);

    // The 8-bit masks replicate the same byte in every lane.
    EXPECT_EQ(ForUtil::mask8(0), 0);
    EXPECT_EQ(ForUtil::mask8(1), 0x01010101);
    EXPECT_EQ(ForUtil::mask8(4), 0x0F0F0F0F);
    EXPECT_EQ(ForUtil::mask8(8), -1);

    // The 16-bit masks replicate the same short in both halves.
    EXPECT_EQ(ForUtil::mask16(0), 0);
    EXPECT_EQ(ForUtil::mask16(1), 0x00010001);
    EXPECT_EQ(ForUtil::mask16(8), 0x00FF00FF);
    EXPECT_EQ(ForUtil::mask16(16), -1);
}

TEST(ForUtilTest, ExpandAndCollapseLanes) {
    std::vector<int32_t> values(ForUtil::BLOCK_SIZE);
    for (size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<int32_t>((i * 37) & 0xFFu);
    }

    std::vector<int32_t> roundTrip = values;
    ForUtil::collapse8(roundTrip);
    // Only the low byte of each value survives the packing.
    ForUtil::expand8(roundTrip);
    EXPECT_EQ(roundTrip, values);

    std::vector<int32_t> wide(ForUtil::BLOCK_SIZE);
    for (size_t i = 0; i < wide.size(); ++i) {
        wide[i] = static_cast<int32_t>((i * 1237) & 0xFFFFu);
    }
    std::vector<int32_t> wideRoundTrip = wide;
    ForUtil::collapse16(wideRoundTrip);
    ForUtil::expand16(wideRoundTrip);
    EXPECT_EQ(wideRoundTrip, wide);
}

TEST(ForUtilTest, RoundTripsEveryBitWidth) {
    ForUtil for_util;
    for (int32_t bits_per_value = 1; bits_per_value <= 32; ++bits_per_value) {
        const std::vector<int32_t> values = make_values(bits_per_value);

        std::vector<uint8_t> bytes(static_cast<size_t>(ForUtil::num_bytes(bits_per_value)) + 16);
        ByteArrayDataOutput out(bytes);
        std::vector<int32_t> to_encode = values;
        auto encoded = for_util.encode(to_encode, bits_per_value, out);
        ASSERT_TRUE(encoded.has_value()) << "bits_per_value=" << bits_per_value;
        ASSERT_EQ(out.get_position(), static_cast<size_t>(ForUtil::num_bytes(bits_per_value)))
                << "bits_per_value=" << bits_per_value;

        ByteArrayDataInput in(std::span<const uint8_t>(bytes.data(), out.get_position()));
        PostingDecodingUtil pdu(in);
        std::vector<int32_t> decoded(ForUtil::BLOCK_SIZE);
        auto result = for_util.decode(bits_per_value, pdu, decoded);
        ASSERT_TRUE(result.has_value()) << "bits_per_value=" << bits_per_value;
        EXPECT_EQ(decoded, values) << "bits_per_value=" << bits_per_value;
        EXPECT_TRUE(in.eof()) << "bits_per_value=" << bits_per_value;
    }
}

TEST(PostingDecodingUtilTest, SplitIntsSameWritesEachPlane) {
    // 4 input ints holding 8 byte lanes each.
    const std::array<int32_t, 4> packed = {0x01020304, 0x11121314, 0x21222324, 0x31323334};
    std::vector<uint8_t> bytes(packed.size() * 4);
    ByteArrayDataOutput out(bytes);
    for (int32_t value : packed) {
        ASSERT_TRUE(out.write_int(value).has_value());
    }

    ByteArrayDataInput in(bytes);
    PostingDecodingUtil pdu(in);
    std::vector<int32_t> b_and_c(32, 0);
    // shift 7 down to 1, one byte lane per plane, stored at c_index = 28.
    auto result = pdu.split_ints_same(4, b_and_c, 7, 1, 0xFF, 28, 0xFF);
    ASSERT_TRUE(result.has_value());

    for (size_t lane = 0; lane < 7; ++lane) {
        const int32_t shift = 7 - static_cast<int32_t>(lane);
        for (size_t i = 0; i < packed.size(); ++i) {
            const int32_t expected = (packed[i] >> shift) & 0xFF;
            EXPECT_EQ(b_and_c[lane * packed.size() + i], expected) << "lane=" << lane << " i=" << i;
        }
    }
    for (size_t i = 0; i < packed.size(); ++i) {
        EXPECT_EQ(b_and_c[28 + i], packed[i] & 0xFF) << "i=" << i;
    }
}

TEST(PostingDecodingUtilTest, SplitIntsDiffWritesEachPlane) {
    const std::array<int32_t, 4> packed = {0x0A0B0C0D, 0x1A1B1C1D, 0x2A2B2C2D, 0x3A3B3C3D};
    std::vector<uint8_t> bytes(packed.size() * 4);
    ByteArrayDataOutput out(bytes);
    for (int32_t value : packed) {
        ASSERT_TRUE(out.write_int(value).has_value());
    }

    ByteArrayDataInput in(bytes);
    PostingDecodingUtil pdu(in);
    std::vector<int32_t> b(7 * packed.size(), 0);
    std::vector<int32_t> c(packed.size(), 0);
    auto result = pdu.split_ints_diff(4, b, 7, 1, 0xFF, c, 0, -1);
    ASSERT_TRUE(result.has_value());

    for (size_t lane = 0; lane < 7; ++lane) {
        const int32_t shift = 7 - static_cast<int32_t>(lane);
        for (size_t i = 0; i < packed.size(); ++i) {
            EXPECT_EQ(b[lane * packed.size() + i], (packed[i] >> shift) & 0xFF) << "lane=" << lane << " i=" << i;
        }
    }
    // c_mask = -1 keeps the raw input values.
    for (size_t i = 0; i < packed.size(); ++i) {
        EXPECT_EQ(c[i], packed[i]) << "i=" << i;
    }
}
