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

module lucene_core:ut.bytes_ref;

import lucene_core;

using namespace lucene;

TEST(BytesRefTest, FromStringAndSlice) {
    BytesRef ref = BytesRef::from_string("hello");
    ASSERT_EQ(ref.length(), 5u);
    EXPECT_EQ(ref.offset(), 0u);
    EXPECT_EQ(ref.utf8_to_string().value(), "hello");
    EXPECT_TRUE(ref.is_valid().has_value());

    BytesRef sliced = BytesRef::from_slice({'a', 'b', 'c', 'd'}, 1, 2);
    EXPECT_EQ(sliced.length(), 2u);
    EXPECT_EQ(sliced.utf8_to_string().value(), "bc");
}

TEST(BytesRefTest, CompareToIsLexicographicOnUnsignedBytes) {
    const BytesRef abc = BytesRef::from_string("abc");
    const BytesRef abd = BytesRef::from_string("abd");
    const BytesRef ab = BytesRef::from_string("ab");

    EXPECT_EQ(abc.compare_to(abd), -1);
    EXPECT_EQ(abd.compare_to(abc), 1);
    EXPECT_EQ(abc.compare_to(BytesRef::from_string("abc")), 0);
    EXPECT_EQ(ab.compare_to(abc), -1);
    EXPECT_EQ(abc.compare_to(ab), 1);

    // Bytes >= 0x80 must sort after ASCII (unsigned comparison).
    const std::vector<uint8_t> high{0x80u};
    const std::vector<uint8_t> ascii{0x7Fu};
    BytesRef high_ref(high);
    BytesRef ascii_ref(ascii);
    EXPECT_EQ(high_ref.compare_to(ascii_ref), 1);
}

TEST(BytesRefTest, DeepCopyAndTakeBytes) {
    BytesRef original = BytesRef::from_string("copy-me");
    auto copy = BytesRef::deep_copy_of(original);
    ASSERT_TRUE(copy.has_value());
    EXPECT_TRUE(std::ranges::equal(copy->as_slice(), original.as_slice()));
    EXPECT_NE(copy->bytes().data(), original.bytes().data());

    auto taken = original.take_bytes();
    EXPECT_EQ(taken.size(), 7u);
    EXPECT_EQ(original.length(), 0u);
}

TEST(BytesRefTest, HexDisplayMatchesRlucene) {
    BytesRef ref = BytesRef::from_slice({'a', 'b'}, 0, 2);
    EXPECT_EQ(ref.to_display_string(), "[61 62]");
}

TEST(BytesRefTest, Utf8Validation) {
    EXPECT_TRUE(BytesRef::from_string("héllo").utf8_to_string().has_value());
    // 0xFF is never valid in UTF-8.
    BytesRef invalid(std::vector<uint8_t>{0xFFu});
    auto result = invalid.utf8_to_string();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind(), ErrorKind::kIllegalArgument);
    // Truncated multi-byte sequence.
    BytesRef truncated(std::vector<uint8_t>{0xE4u, 0xB8u});
    EXPECT_FALSE(truncated.utf8_to_string().has_value());
}

TEST(BytesRefTest, HashCodeIsStable) {
    const BytesRef a = BytesRef::from_string("term");
    const BytesRef b = BytesRef::from_string("term");
    const BytesRef c = BytesRef::from_string("term2");
    EXPECT_EQ(a.hash_code(), b.hash_code());
    EXPECT_NE(a.hash_code(), c.hash_code());
}

TEST(BytesRefBuilderTest, AppendAndCopy) {
    BytesRefBuilder builder;
    ASSERT_TRUE(builder.append_byte('a').has_value());
    ASSERT_TRUE(builder.append(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>("bc"), 2)).has_value());
    EXPECT_EQ(builder.length(), 3u);
    EXPECT_EQ(builder.get_bytes_ref().utf8_to_string().value(), "abc");

    ASSERT_TRUE(builder.copy_chars("hello").has_value());
    EXPECT_EQ(builder.get_bytes_ref().utf8_to_string().value(), "hello");

    // copy_chars with explicit range.
    const std::u32string chars = U"xyz";
    ASSERT_TRUE(builder.copy_chars(std::span<const char32_t>(chars.data(), chars.size()), 1, 2).has_value());
    EXPECT_EQ(builder.get_bytes_ref().utf8_to_string().value(), "yz");

    builder.clear();
    EXPECT_EQ(builder.length(), 0u);
    auto copy = builder.get_bytes_ref_copy();
    ASSERT_TRUE(copy.has_value());
    EXPECT_EQ(copy->length(), 0u);
}
