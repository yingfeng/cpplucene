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

module lucene_core:ut.bit_util;

import lucene_core;

using namespace lucene;

TEST(BitUtilTest, BigAndLittleEndianRoundTrip) {
    std::array<uint8_t, 8> buffer{};

    BitUtil::set_i16_le(buffer, 0, 0x1234);
    EXPECT_EQ(buffer[0], 0x34);
    EXPECT_EQ(buffer[1], 0x12);
    EXPECT_EQ(BitUtil::get_i16_le(buffer, 0), 0x1234);

    BitUtil::set_i16_be(buffer, 0, 0x1234);
    EXPECT_EQ(buffer[0], 0x12);
    EXPECT_EQ(buffer[1], 0x34);
    EXPECT_EQ(BitUtil::get_i16_be(buffer, 0), 0x1234);

    BitUtil::set_i32_le(buffer, 0, 0x12345678);
    EXPECT_EQ(BitUtil::get_i32_le(buffer, 0), 0x12345678);
    BitUtil::set_i32_be(buffer, 0, 0x12345678);
    EXPECT_EQ(BitUtil::get_i32_be(buffer, 0), 0x12345678);

    BitUtil::set_i64_le(buffer, 0, 0x0102030405060708LL);
    EXPECT_EQ(BitUtil::get_i64_le(buffer, 0), 0x0102030405060708LL);
    BitUtil::set_i64_be(buffer, 0, 0x0102030405060708LL);
    EXPECT_EQ(BitUtil::get_i64_be(buffer, 0), 0x0102030405060708LL);

    // Negative values must round-trip as well.
    BitUtil::set_i32_le(buffer, 0, -2);
    EXPECT_EQ(BitUtil::get_i32_le(buffer, 0), -2);
}

TEST(BitUtilTest, PartialLengthWrites) {
    std::array<uint8_t, 8> buffer{};
    BitUtil::set_i32_be(buffer, 0, 0x12345678, 2);
    EXPECT_EQ(buffer[0], 0x12);
    EXPECT_EQ(buffer[1], 0x34);
    EXPECT_EQ(buffer[2], 0x00);

    BitUtil::set_i64_le(buffer, 0, 0x1122334455667788LL, 3);
    EXPECT_EQ(buffer[0], 0x88);
    EXPECT_EQ(buffer[1], 0x77);
    EXPECT_EQ(buffer[2], 0x66);
}

TEST(BitUtilTest, PowerOfTwoHelpers) {
    EXPECT_EQ(BitUtil::next_highest_power_of_two_with_i32(1), 1);
    EXPECT_EQ(BitUtil::next_highest_power_of_two_with_i32(2), 2);
    EXPECT_EQ(BitUtil::next_highest_power_of_two_with_i32(3), 4);
    EXPECT_EQ(BitUtil::next_highest_power_of_two_with_i32(1000), 1024);
    EXPECT_EQ(BitUtil::next_highest_power_of_two_with_i64(1LL << 40), 1LL << 40);
    EXPECT_EQ(BitUtil::next_highest_power_of_two_with_i64((1LL << 40) + 1), 1LL << 41);

    EXPECT_TRUE(BitUtil::is_zero_or_power_of_two(0));
    EXPECT_TRUE(BitUtil::is_zero_or_power_of_two(1));
    EXPECT_TRUE(BitUtil::is_zero_or_power_of_two(1024));
    EXPECT_FALSE(BitUtil::is_zero_or_power_of_two(3));
    EXPECT_FALSE(BitUtil::is_zero_or_power_of_two(-1));
}

TEST(BitUtilTest, ZigZag) {
    for (int32_t value : {0, -1, 1, 2, -2, 63, -64, 1234567, -1234567, std::numeric_limits<int32_t>::max(),
                          std::numeric_limits<int32_t>::min()}) {
        const int32_t encoded = BitUtil::zig_zag_encode_i32(value);
        EXPECT_EQ(BitUtil::zig_zag_decode_i32(static_cast<uint32_t>(encoded)), value);
    }
    for (int64_t value : {int64_t{0}, int64_t{-1}, int64_t{1}, int64_t{-1234567890123LL},
                          std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()}) {
        const int64_t encoded = BitUtil::zig_zag_encode_i64(value);
        EXPECT_EQ(BitUtil::zig_zag_decode_i64(static_cast<uint64_t>(encoded)), value);
    }
    EXPECT_EQ(BitUtil::zig_zag_encode_i32(0), 0);
    EXPECT_EQ(BitUtil::zig_zag_encode_i32(-1), 1);
    // Standard zig-zag: 0 -> 0, -1 -> 1, 1 -> 2.
    EXPECT_EQ(BitUtil::zig_zag_encode_i32(1), 2);
}

TEST(BitUtilTest, InterleaveAndFlipFlop) {
    // Interleaved morton code of (1, 0) is 1, of (0, 1) is 2.
    EXPECT_EQ(BitUtil::interleave(1, 0), 1);
    EXPECT_EQ(BitUtil::interleave(0, 1), 2);
    EXPECT_EQ(BitUtil::interleave(1, 1), 3);
    EXPECT_EQ(BitUtil::interleave(0, 0), 0);
    // Swapping odd and even bits undoes the interleaving of a single bit pair.
    EXPECT_EQ(BitUtil::flip_flop(1), 2);
    EXPECT_EQ(BitUtil::flip_flop(2), 1);
    EXPECT_EQ(BitUtil::flip_flop(3), 3);
}

TEST(BitUtilTest, FloatBitsCanonicalization) {
    EXPECT_EQ(BitUtil::float_to_int_bits(1.0F), 0x3F800000);
    EXPECT_EQ(static_cast<uint32_t>(BitUtil::float_to_int_bits(std::numeric_limits<float>::quiet_NaN())),
              BitUtil::FLOAT_NAN_BITS);
    EXPECT_EQ(BitUtil::double_to_long_bits(1.0), 0x3FF0000000000000LL);
    EXPECT_EQ(static_cast<uint64_t>(BitUtil::double_to_long_bits(std::numeric_limits<double>::quiet_NaN())),
              BitUtil::DOUBLE_NAN_BITS);
}
