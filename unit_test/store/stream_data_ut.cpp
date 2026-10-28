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
#include <expected>
#include <sstream>
#include <string>
#include <vector>

module lucene_core:ut.stream_data;

import lucene_core;

using namespace lucene;

TEST(StreamDataTest, WritesAndReadsScalars) {
    std::stringstream stream;
    OutputStreamDataOutput output(stream);

    ASSERT_TRUE(output.write_byte(0x7F).has_value());
    ASSERT_TRUE(output.write_int(0x01020304).has_value());
    ASSERT_TRUE(output.write_long(0x0102030405060708LL).has_value());
    const std::vector<uint8_t> tail{1, 2, 3};
    ASSERT_TRUE(output.write_bytes(tail).has_value());

    // Little endian, seven bytes of the byte plus the two scalars, then the tail.
    InputStreamDataInput input(stream);
    EXPECT_EQ(input.read_byte().value(), 0x7F);
    EXPECT_EQ(input.read_int().value(), 0x01020304);
    EXPECT_EQ(input.read_long().value(), 0x0102030405060708LL);
    std::vector<uint8_t> read_tail(3);
    ASSERT_TRUE(input.read_bytes(read_tail).has_value());
    EXPECT_EQ(read_tail, tail);

    // Reading past the end reports an I/O error instead of returning zeros.
    EXPECT_FALSE(input.read_byte().has_value());
}

TEST(StreamDataTest, SkipsAndRejectsBadArguments) {
    std::stringstream stream;
    OutputStreamDataOutput output(stream);
    ASSERT_TRUE(output.write_int(11).has_value());
    ASSERT_TRUE(output.write_int(22).has_value());

    InputStreamDataInput input(stream);
    ASSERT_TRUE(input.skip_bytes(4).has_value());
    EXPECT_EQ(input.read_int().value(), 22);
    EXPECT_FALSE(input.skip_bytes(10).has_value());
    EXPECT_FALSE(input.skip_bytes(-1).has_value());

    EXPECT_EQ(output.to_string(), "OutputStreamDataOutput");
    EXPECT_EQ(input.to_string(), "InputStreamDataInput");
}

TEST(StreamDataTest, RejectsOutOfRangeSlices) {
    std::stringstream stream;
    OutputStreamDataOutput output(stream);
    const std::vector<uint8_t> data{1, 2, 3};
    EXPECT_FALSE(output.write_bytes(data, 2, 5).has_value());
    EXPECT_TRUE(output.write_bytes(data, 1, 2).has_value());
    EXPECT_EQ(stream.str(), std::string("\x02\x03", 2));

    std::vector<uint8_t> empty;
    EXPECT_TRUE(output.write_bytes(empty).has_value());
}
