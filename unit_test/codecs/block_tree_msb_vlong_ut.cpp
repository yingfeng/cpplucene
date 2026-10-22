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
#include <vector>

module lucene_core:ut.block_tree_msb_vlong;

import lucene_core;

using namespace lucene;

namespace {

std::vector<uint8_t> write_value(int64_t value) {
    GrowableByteArrayDataOutput out;
    EXPECT_TRUE(write_msb_vlong(value, out).has_value());
    return out.take_bytes();
}

int64_t read_value(const std::vector<uint8_t> &bytes) {
    ByteArrayDataInput in{bytes};
    auto read = read_msb_vlong(in);
    EXPECT_TRUE(read.has_value());
    EXPECT_EQ(in.get_position(), in.length());
    return read.value_or(-1);
}

} // namespace

TEST(BlockTreeMsbVlongTest, RoundTripsValues) {
    const std::vector<int64_t> values{0,   1,    2,     127,     128,      129,
                                      255, 1000, 0x7FFF, 1 << 20, int64_t{1} << 40,
                                      std::numeric_limits<int64_t>::max()};
    for (int64_t value : values) {
        EXPECT_EQ(read_value(write_value(value)), value) << "value=" << value;
    }
}

TEST(BlockTreeMsbVlongTest, KeepsZeroBitsInTheMostSignificantByte) {
    // 0x7FFF is stored as [0x81, 0xFF, 0x7F] (and not [0xFF, 0xFF, 0x40]).
    const std::vector<uint8_t> bytes = write_value(0x7FFF);
    const std::vector<uint8_t> expected{0x81, 0xFF, 0x7F};
    EXPECT_EQ(bytes, expected);
}

TEST(BlockTreeMsbVlongTest, SingleByteForSmallValues) {
    EXPECT_EQ(write_value(0), std::vector<uint8_t>({0x00}));
    EXPECT_EQ(write_value(1), std::vector<uint8_t>({0x01}));
    EXPECT_EQ(write_value(127), std::vector<uint8_t>({0x7F}));
    // 128 needs a second byte, whose first byte carries the continuation bit.
    EXPECT_EQ(write_value(128), std::vector<uint8_t>({0x81, 0x00}));
}

TEST(BlockTreeMsbVlongTest, SharedPrefixForSiblingPointers) {
    // Siblings in a block are close together, so their encodings share a prefix.
    const std::vector<uint8_t> first = write_value(0x0100);
    const std::vector<uint8_t> second = write_value(0x0101);
    ASSERT_GE(first.size(), 2u);
    ASSERT_EQ(first.size(), second.size());
    EXPECT_EQ(first[0], second[0]);
}
