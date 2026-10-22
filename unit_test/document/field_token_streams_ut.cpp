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
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.field_token_streams;

import lucene_core;

using namespace lucene;

namespace {

/// Reads every term of `stream` after resetting it.
std::vector<std::string> drain(TokenStream &stream) {
    std::vector<std::string> terms;
    EXPECT_TRUE(stream.reset().has_value());
    auto *terms_attribute = stream.attributes().add_attribute<CharTermAttribute>();
    for (;;) {
        auto has_next = stream.increment_token();
        EXPECT_TRUE(has_next.has_value()) << (has_next ? "" : has_next.error().to_string());
        if (!has_next.has_value() || !has_next.value()) {
            break;
        }
        terms.emplace_back(terms_attribute->text());
    }
    EXPECT_TRUE(stream.end().has_value());
    return terms;
}

FieldType indexed_type(bool tokenized) {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_tokenized(tokenized).has_value());
    return type;
}

} // namespace

TEST(FieldTokenStreamsTest, Utf16LengthCountsSurrogatePairs) {
    auto ascii = utf16_length("abc");
    ASSERT_TRUE(ascii.has_value());
    EXPECT_EQ(ascii.value(), 3);

    // U+00E9 (é) is one UTF-16 unit, U+1F600 (emoji) is two.
    auto mixed = utf16_length("a\u00E9\u{1F600}");
    ASSERT_TRUE(mixed.has_value());
    EXPECT_EQ(mixed.value(), 4);

    auto empty = utf16_length("");
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty.value(), 0);
}

TEST(FieldTokenStreamsTest, StringTokenStreamEmitsOneToken) {
    StringTokenStream stream;
    ASSERT_TRUE(stream.set_value("Hello").has_value());
    EXPECT_EQ(stream.end_offset(), 5);

    auto *terms = stream.attributes().add_attribute<CharTermAttribute>();
    auto *offsets = stream.attributes().add_attribute<OffsetAttribute>();
    auto *positions = stream.attributes().add_attribute<PositionIncrementAttribute>();

    ASSERT_TRUE(stream.reset().has_value());
    auto first = stream.increment_token();
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first.value());
    EXPECT_EQ(terms->text(), "Hello");
    EXPECT_EQ(offsets->start_offset(), 0);
    EXPECT_EQ(offsets->end_offset(), 5);
    EXPECT_EQ(positions->position_increment(), 1);

    auto second = stream.increment_token();
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second.value());

    // `end` restores the final offset of the whole value.
    ASSERT_TRUE(stream.end().has_value());
    EXPECT_EQ(offsets->start_offset(), 5);
    EXPECT_EQ(offsets->end_offset(), 5);

    // The stream can be reused for the next document.
    ASSERT_TRUE(stream.set_value("bye").has_value());
    ASSERT_TRUE(stream.reset().has_value());
    auto reused = stream.increment_token();
    ASSERT_TRUE(reused.has_value());
    EXPECT_TRUE(reused.value());
    EXPECT_EQ(terms->text(), "bye");
}

TEST(FieldTokenStreamsTest, StringTokenStreamRequiresAValue) {
    StringTokenStream stream;
    auto token = stream.increment_token();
    ASSERT_FALSE(token.has_value());
    EXPECT_EQ(token.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(FieldTokenStreamsTest, BinaryTokenStreamEmitsOneToken) {
    BinaryTokenStream stream;
    const std::vector<uint8_t> value{0x01, 0x02, 0xff};
    stream.set_value(value);

    auto *terms = stream.attributes().add_attribute<BytesTermAttribute>();
    auto *positions = stream.attributes().add_attribute<PositionIncrementAttribute>();

    auto first = stream.increment_token();
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first.value());
    const auto read = terms->bytes_ref();
    EXPECT_EQ(std::vector<uint8_t>(read.begin(), read.end()), value);
    EXPECT_EQ(positions->position_increment(), 1);

    auto second = stream.increment_token();
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second.value());
}

TEST(FieldTokenStreamsTest, FieldReturnsNoStreamWhenNotIndexed) {
    FieldType type;
    ASSERT_TRUE(type.set_stored(true).has_value());
    Field field("title", std::string("hello"), std::move(type));

    KeywordAnalyzer analyzer;
    auto stream = field.token_stream(analyzer);
    ASSERT_TRUE(stream.has_value());
    EXPECT_TRUE(stream.value() == nullptr);
}

TEST(FieldTokenStreamsTest, NonTokenizedFieldUsesAStringTokenStream) {
    Field field("id", std::string("doc-1"), indexed_type(/*tokenized=*/false));

    KeywordAnalyzer analyzer;
    auto stream = field.token_stream(analyzer);
    ASSERT_TRUE(stream.has_value());
    ASSERT_TRUE(stream.value() != nullptr);
    EXPECT_EQ(drain(*stream.value()), (std::vector<std::string>{"doc-1"}));

    // The same stream instance is reused, and the value follows the field.
    ASSERT_TRUE(field.set_string_value("doc-2").has_value());
    auto again = field.token_stream(analyzer);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again.value(), stream.value());
    EXPECT_EQ(drain(*again.value()), (std::vector<std::string>{"doc-2"}));
}

TEST(FieldTokenStreamsTest, NonTokenizedBinaryFieldUsesABinaryTokenStream) {
    Field field("blob", std::vector<uint8_t>{0x09, 0x0a}, indexed_type(/*tokenized=*/false));

    KeywordAnalyzer analyzer;
    auto stream = field.token_stream(analyzer);
    ASSERT_TRUE(stream.has_value());
    ASSERT_TRUE(stream.value() != nullptr);

    auto *terms = stream.value()->attributes().add_attribute<BytesTermAttribute>();
    ASSERT_TRUE(stream.value()->reset().has_value());
    auto token = stream.value()->increment_token();
    ASSERT_TRUE(token.has_value());
    ASSERT_TRUE(token.value());
    EXPECT_EQ(terms->bytes_ref().size(), 2);
}

TEST(FieldTokenStreamsTest, TokenizedFieldUsesTheAnalyzer) {
    Field field("title", std::string("Hello World"), indexed_type(/*tokenized=*/true));

    WhitespaceAnalyzer analyzer;
    auto stream = field.token_stream(analyzer);
    ASSERT_TRUE(stream.has_value());
    ASSERT_TRUE(stream.value() != nullptr);
    EXPECT_EQ(drain(*stream.value()), (std::vector<std::string>{"Hello", "World"}));
}

TEST(FieldTokenStreamsTest, CopiesStartWithoutTokenStream) {
    Field original("id", std::string("doc-1"), indexed_type(/*tokenized=*/false));
    KeywordAnalyzer analyzer;
    auto first = original.token_stream(analyzer);
    ASSERT_TRUE(first.has_value());

    Field copy = original;
    auto copied_stream = copy.token_stream(analyzer);
    ASSERT_TRUE(copied_stream.has_value());
    EXPECT_NE(copied_stream.value(), first.value());
    EXPECT_EQ(drain(*copied_stream.value()), (std::vector<std::string>{"doc-1"}));
}
