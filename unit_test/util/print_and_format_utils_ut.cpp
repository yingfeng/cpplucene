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

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

module lucene_core:ut.print_and_format_utils;

import lucene_core;

using namespace lucene;

TEST(ToStringUtilsTest, RendersLongsInHex) {
    EXPECT_EQ(ToStringUtils::long_hex(0), "0x0000000000000000");
    EXPECT_EQ(ToStringUtils::long_hex(0xdeadbeef), "0x00000000deadbeef");
    EXPECT_EQ(ToStringUtils::long_hex(UINT64_MAX), "0xffffffffffffffff");
}

TEST(ToStringUtilsTest, RendersBytesRefs) {
    BytesRef empty;
    EXPECT_EQ(ToStringUtils::bytes_ref_to_string(empty), "null");

    BytesRef text;
    text.copy_from_slice(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>("hello"), 5));
    EXPECT_EQ(ToStringUtils::bytes_ref_to_string(text), "hello");

    // An invalid UTF-8 byte falls back to the reference implementation's rendering.
    const std::vector<uint8_t> bogus{0xff, 0x00};
    EXPECT_EQ(ToStringUtils::bytes_ref_to_string_from_bytes(bogus), "[ff 00]");
}

TEST(StrictStringTokenizerTest, SplitsAndKeepsEmptyTokens) {
    StrictStringTokenizer tokenizer("1.2..4", '.');
    EXPECT_TRUE(tokenizer.has_more_tokens());
    EXPECT_EQ(tokenizer.next_token().value(), "1");
    EXPECT_EQ(tokenizer.next_token().value(), "2");
    EXPECT_EQ(tokenizer.next_token().value(), "");
    EXPECT_EQ(tokenizer.next_token().value(), "4");
    // The input is spent: the reference reports an error rather than an empty
    // string, and repeated calls keep reporting it.
    EXPECT_FALSE(tokenizer.has_more_tokens());
    auto spent = tokenizer.next_token();
    ASSERT_FALSE(spent.has_value());
    EXPECT_EQ(spent.error().kind(), ErrorKind::kIllegalState);
    EXPECT_FALSE(tokenizer.next_token().has_value());
}

TEST(StrictStringTokenizerTest, HandlesATrailingDelimiter) {
    StrictStringTokenizer tokenizer("a.", '.');
    EXPECT_EQ(tokenizer.next_token().value(), "a");
    // The position moves past the delimiter, i.e. the input counts as spent even
    // though the trailing token would be empty.
    auto trailing = tokenizer.next_token();
    ASSERT_FALSE(trailing.has_value());
    EXPECT_EQ(trailing.error().kind(), ErrorKind::kIllegalState);
}

TEST(PrintStreamInfoStreamTest, WritesOneLinePerMessage) {
    std::ostringstream out;
    PrintStreamInfoStream stream(out, 7);
    EXPECT_TRUE(stream.is_enabled("IW"));
    ASSERT_TRUE(stream.message("IW", "started").has_value());
    ASSERT_TRUE(stream.message("IFD", "flushed").has_value());

    const std::string text = out.str();
    EXPECT_NE(text.find("IW 7 ["), std::string::npos);
    EXPECT_NE(text.find("]: started"), std::string::npos);
    EXPECT_NE(text.find("IFD 7 ["), std::string::npos);
    EXPECT_NE(text.find("]: flushed"), std::string::npos);

    // Two lines, each ending in a newline.
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 2);
    // The timestamp is ISO-8601 in UTC.
    const std::string timestamp = PrintStreamInfoStream::get_timestamp();
    EXPECT_EQ(timestamp.size(), 20U);
    EXPECT_EQ(timestamp[4], '-');
    EXPECT_EQ(timestamp[10], 'T');
    EXPECT_EQ(timestamp.back(), 'Z');

    ASSERT_TRUE(stream.close().has_value());
    // A closed stream drops messages instead of failing.
    ASSERT_TRUE(stream.message("IW", "ignored").has_value());
    EXPECT_EQ(out.str().find("ignored"), std::string::npos);
    ASSERT_TRUE(stream.close().has_value());
}
