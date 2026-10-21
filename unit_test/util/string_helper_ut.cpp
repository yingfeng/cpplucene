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

module clucene:ut.string_helper;

import clucene;

using namespace clucene;

namespace {

std::span<const uint8_t> as_bytes(std::string_view text) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

} // namespace

TEST(StringHelperTest, StartsWithAndEndsWith) {
    EXPECT_TRUE(StringHelper::starts_with(as_bytes("foobar"), as_bytes("foo")));
    EXPECT_FALSE(StringHelper::starts_with(as_bytes("foobar"), as_bytes("bar")));
    EXPECT_TRUE(StringHelper::starts_with(as_bytes("foobar"), as_bytes("")));
    EXPECT_FALSE(StringHelper::starts_with(as_bytes("fo"), as_bytes("foo")));

    EXPECT_TRUE(StringHelper::ends_with(as_bytes("foobar"), as_bytes("bar")));
    EXPECT_FALSE(StringHelper::ends_with(as_bytes("foobar"), as_bytes("foo")));
    EXPECT_TRUE(StringHelper::ends_with(as_bytes("foobar"), as_bytes("")));
    EXPECT_FALSE(StringHelper::ends_with(as_bytes("ar"), as_bytes("bar")));
}

TEST(StringHelperTest, BytesDifference) {
    auto difference = StringHelper::bytes_difference(as_bytes("abc"), as_bytes("abd"));
    ASSERT_TRUE(difference.has_value());
    EXPECT_EQ(difference.value(), 2u);

    auto key_length = StringHelper::sort_key_length(as_bytes("abc"), as_bytes("abd"));
    ASSERT_TRUE(key_length.has_value());
    EXPECT_EQ(key_length.value(), 3u);

    // rlucene only reports an error when the two terms are fully identical
    // (`mismatch < 0`); an out-of-order pair is not validated here.
    auto reversed = StringHelper::bytes_difference(as_bytes("abd"), as_bytes("abc"));
    ASSERT_TRUE(reversed.has_value());
    EXPECT_EQ(reversed.value(), 2u);
    auto identical = StringHelper::bytes_difference(as_bytes("abc"), as_bytes("abc"));
    ASSERT_FALSE(identical.has_value());
    EXPECT_EQ(identical.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(StringHelperTest, MurmurHash3KnownValues) {
    // Reference values from yonik/java_util (the implementation rlucene uses).
    EXPECT_EQ(StringHelper::murmurhash3_x86_32(as_bytes(""), 0), 0);
    EXPECT_EQ(static_cast<uint32_t>(StringHelper::murmurhash3_x86_32(as_bytes("hello"), 0)), 0x248bfa47u);
    // Known value with the classic Lucene seed.
    EXPECT_EQ(static_cast<uint32_t>(StringHelper::murmurhash3_x86_32(as_bytes("The quick brown fox jumps over the lazy dog"),
                                                                     static_cast<int32_t>(0x9747b28c))),
              0x2fa826cd);
    // Same input, same seed => same hash.
    EXPECT_EQ(StringHelper::murmurhash3_x86_32(as_bytes("abcdefg"), 42),
              StringHelper::murmurhash3_x86_32(as_bytes("abcdefg"), 42));
}

TEST(StringHelperTest, IdToStringIsBase36) {
    std::array<uint8_t, StringHelper::ID_LENGTH> zero{};
    EXPECT_EQ(StringHelper::id_to_string(std::span<const uint8_t>(zero)), "0");

    std::array<uint8_t, StringHelper::ID_LENGTH> value{};
    value[15] = 35; // 'z' in base 36.
    EXPECT_EQ(StringHelper::id_to_string(std::span<const uint8_t>(value)), "z");

    value[15] = 36;
    EXPECT_EQ(StringHelper::id_to_string(std::span<const uint8_t>(value)), "10");

    EXPECT_EQ(StringHelper::id_to_string(std::span<const uint8_t>()), "(null)");
}

TEST(StringHelperTest, RandomIdIsMonotonic) {
    const auto first = StringHelper::random_id();
    const auto second = StringHelper::random_id();
    EXPECT_NE(first, second);
    // The counter increments big-endian, so the last byte advances.
    EXPECT_LT(first, second);
}
