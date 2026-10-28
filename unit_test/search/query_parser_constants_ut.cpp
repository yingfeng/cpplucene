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

#include <string>

module lucene_core:ut.query_parser_constants;

import lucene_core;

using namespace lucene;

TEST(QueryParserConstantsTest, TokenIdsMatchTheReferenceVocabulary) {
    // The ids are the ones a JFlex-generated lexer would hand out, so a lexer built
    // on top of them stays compatible with the reference numbers.
    EXPECT_EQ(query_syntax::EOF_TOKEN, 0);
    EXPECT_EQ(query_syntax::NUM_CHAR, 1);
    EXPECT_EQ(query_syntax::QUOTED_CHAR, 6);
    EXPECT_EQ(query_syntax::AND, 8);
    EXPECT_EQ(query_syntax::NOT, 10);
    EXPECT_EQ(query_syntax::LPAREN, 14);
    EXPECT_EQ(query_syntax::QUOTED, 19);
    EXPECT_EQ(query_syntax::TERM, 20);
    EXPECT_EQ(query_syntax::REGEXPTERM, 24);
    EXPECT_EQ(query_syntax::RANGEIN_START, 25);
    EXPECT_EQ(query_syntax::RANGE_TO, 28);
    EXPECT_EQ(query_syntax::RANGE_GOOP, 32);
    // Kind 7 is deliberately unnamed, exactly like in the reference implementation.
    EXPECT_EQ(query_syntax::token_image(7), "<token of kind 7>");
}

TEST(QueryParserConstantsTest, LexicalStatesAreDistinct) {
    EXPECT_NE(query_syntax::BOOST_LEXICAL_STATE, query_syntax::RANGE_LEXICAL_STATE);
    EXPECT_NE(query_syntax::RANGE_LEXICAL_STATE, query_syntax::DEFAULT_LEXICAL_STATE);
    EXPECT_EQ(query_syntax::DEFAULT_LEXICAL_STATE, 2);
}

TEST(QueryParserConstantsTest, EveryKindHasAnImage) {
    ASSERT_EQ(query_syntax::token_image_count(), 33U);
    EXPECT_EQ(query_syntax::token_image(query_syntax::AND), "<AND>");
    EXPECT_EQ(query_syntax::token_image(query_syntax::PLUS), "\"+\"");
    EXPECT_EQ(query_syntax::token_image(query_syntax::TERM), "<TERM>");
    EXPECT_EQ(query_syntax::token_image(query_syntax::RANGE_GOOP), "<RANGE_GOOP>");
    for (size_t kind = 0; kind < query_syntax::token_image_count(); ++kind) {
        EXPECT_FALSE(query_syntax::token_image(static_cast<int32_t>(kind)).empty()) << kind;
    }
    // Out of range kinds answer a placeholder instead of reading out of bounds.
    EXPECT_EQ(query_syntax::token_image(-1), "<unknown>");
    EXPECT_EQ(query_syntax::token_image(33), "<unknown>");
}
