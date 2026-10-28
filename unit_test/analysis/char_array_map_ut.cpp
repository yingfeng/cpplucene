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

#include <set>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.char_array_map;

import lucene_core;

using namespace lucene;

namespace {

std::vector<std::pair<std::string, int>> entries() {
    return {{"one", 1}, {"two", 2}, {"three", 3}};
}

} // namespace

TEST(CharArrayMapTest, DecodesAndEncodesUtf8) {
    const std::string text = "a\xC3\xA4\xE4\xB8\xAD\xF0\x9F\x98\x80"; // a, ä, 中, 😀
    const auto code_points = CharArrayMap<int>::decode(text);
    ASSERT_EQ(code_points.size(), 4U);
    EXPECT_EQ(code_points[0], U'a');
    EXPECT_EQ(code_points[1], 0x00E4);
    EXPECT_EQ(code_points[2], 0x4E2D);
    EXPECT_EQ(code_points[3], 0x1F600);
    EXPECT_EQ(CharArrayMap<int>::utf8(code_points), text);

    // Malformed input degrades to U+FFFD instead of failing.
    const auto broken = CharArrayMap<int>::decode("a\xFF");
    ASSERT_EQ(broken.size(), 2U);
    EXPECT_EQ(broken[0], U'a');
    EXPECT_EQ(broken[1], 0xFFFD);

    // A surrogate cannot be encoded.
    EXPECT_EQ(CharArrayMap<int>::utf8(std::u32string(1, 0xD800)), "\xEF\xBF\xBD");
}

TEST(CharArrayMapTest, StoresAndLooksUpUtf8Keys) {
    CharArrayMap<int> map(false);
    EXPECT_TRUE(map.is_empty());

    EXPECT_FALSE(map.put_str("one", 1).has_value());
    EXPECT_FALSE(map.put_str("two", 2).has_value());
    // `put` returns the value it replaced.
    ASSERT_EQ(map.put_str("one", 10).value(), 1);

    ASSERT_NE(map.get_str("one"), nullptr);
    EXPECT_EQ(*map.get_str("one"), 10);
    EXPECT_EQ(map.get_str("missing"), nullptr);
    EXPECT_EQ(map.size(), 2U);
    EXPECT_TRUE(map.contains_key_str("two"));
    EXPECT_FALSE(map.contains_key_str("three"));

    map.put_all_str(entries());
    EXPECT_EQ(map.size(), 3U);
    EXPECT_EQ(*map.get_str("three"), 3);

    const auto text = map.to_string([](int value) { return std::to_string(value); });
    EXPECT_NE(text.find("one=1"), std::string::npos);
    EXPECT_NE(text.find("three=3"), std::string::npos);

    map.clear();
    EXPECT_TRUE(map.is_empty());
}

TEST(CharArrayMapTest, CaseSensitiveAndInsensitiveKeys) {
    CharArrayMap<int> sensitive(false);
    (void)sensitive.put_str("Abc", 1);
    EXPECT_FALSE(sensitive.contains_key_str("abc"));
    EXPECT_TRUE(sensitive.contains_key_str("Abc"));

    CharArrayMap<int> insensitive(true);
    EXPECT_TRUE(insensitive.ignore_case());
    (void)insensitive.put_str("Abc", 1);
    EXPECT_TRUE(insensitive.contains_key_str("abc"));
    EXPECT_TRUE(insensitive.contains_key_str("ABC"));
    ASSERT_NE(insensitive.get_str("aBC"), nullptr);
    EXPECT_EQ(*insensitive.get_str("aBC"), 1);

    // Non-ASCII folding follows the shared approximation.
    (void)insensitive.put_str("\xC3\x84" "rger", 7); // Ärger
    ASSERT_NE(insensitive.get_str("\xC3\xA4" "rger"), nullptr); // ärger
    EXPECT_EQ(*insensitive.get_str("\xC3\xA4" "rger"), 7);
    (void)insensitive.put_str("\xCE\xA3", 8); // Σ
    ASSERT_NE(insensitive.get_str("\xCF\x83"), nullptr); // σ
    EXPECT_EQ(*insensitive.get_str("\xCF\x83"), 8);

    // The key is stored folded, so the stored key is the lower case one.
    const auto keys = insensitive.keys();
    bool found_folded = false;
    for (const auto &key : keys) {
        if (CharArrayMap<int>::utf8(key) == "abc") {
            found_folded = true;
        }
    }
    EXPECT_TRUE(found_folded);
}

TEST(CharArrayMapTest, SlicesByCodePoint) {
    CharArrayMap<int> map(true);
    (void)map.put_str("abcdef", 5);

    // A code point slice has to match a whole key; "xabcde" is not one.
    EXPECT_EQ(map.get_str("xxabcdefxx", 1, 6), nullptr);
    (void)map.put_str("cde", 3);
    ASSERT_NE(map.get_str("abcdef", 2, 3), nullptr);
    EXPECT_EQ(*map.get_str("abcdef", 2, 3), 3);
    // Out of range slices are rejected instead of reading past the end.
    EXPECT_EQ(map.get_str("abcdef", 4, 10), nullptr);
}

TEST(CharArraySetTest, AddsAndLooksUp) {
    CharArraySet set(true);
    EXPECT_TRUE(set.is_empty());
    EXPECT_FALSE(set.add_str("The"));
    EXPECT_TRUE(set.add_str("the")); // already present, case-insensitively
    EXPECT_EQ(set.size(), 1U);
    EXPECT_TRUE(set.contains_key_str("THE"));
    EXPECT_TRUE(set.contains_key_str("the"));
    EXPECT_EQ(set.to_string(), "[the]");

    set.add_all(std::vector<std::string>{"of", "and"});
    EXPECT_EQ(set.size(), 3U);
    EXPECT_TRUE(set.contains_key_str("And"));

    // A code point slice of a longer term: "which" is not a key until added.
    EXPECT_FALSE(set.contains_key_str("whichever", 0, 5));
    EXPECT_FALSE(set.add_str("which"));
    EXPECT_TRUE(set.contains_key_str("whichever", 0, 5));
    EXPECT_TRUE(set.contains_key_str("WHICHEVER", 0, 5));
    EXPECT_FALSE(set.contains_key_str("whichever", 1, 5)); // "hiche"

    set.clear();
    EXPECT_TRUE(set.is_empty());
}

TEST(CharArraySetTest, BridgesAStopWordSet) {
    const std::set<std::string, std::less<>> words{"the", "a", "an"};
    const CharArraySet set = to_char_array_set(words);
    EXPECT_EQ(set.size(), 3U);
    EXPECT_TRUE(set.contains_key_str("THE"));
    EXPECT_TRUE(set.contains_key_str("An"));
    EXPECT_FALSE(set.contains_key_str("and"));

    // Case-sensitive bridging keeps the original spelling.
    const CharArraySet sensitive = to_char_array_set(words, false);
    EXPECT_TRUE(sensitive.contains_key_str("the"));
    EXPECT_FALSE(sensitive.contains_key_str("THE"));
}

TEST(CharToLowerTest, FoldsTheCoveredScripts) {
    EXPECT_EQ(char_to_lower(U'A'), U'a');
    EXPECT_EQ(char_to_lower(U'Z'), U'z');
    EXPECT_EQ(char_to_lower(U'1'), U'1');
    EXPECT_EQ(char_to_lower(0x00C4), 0x00E4); // Ä -> ä
    EXPECT_EQ(char_to_lower(0x00D7), 0x00D7); // × is not a letter
    EXPECT_EQ(char_to_lower(0x0391), 0x03B1); // Α -> α
    EXPECT_EQ(char_to_lower(0x0410), 0x0430); // А -> а
    EXPECT_EQ(char_to_lower(0x4E2D), 0x4E2D); // 中 has no case
}
