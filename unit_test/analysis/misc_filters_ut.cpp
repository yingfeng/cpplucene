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
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.misc_filters;

import lucene_core;

using namespace lucene;

namespace {

std::vector<std::string> analyze(Analyzer &analyzer, std::string_view text) {
    std::vector<std::string> tokens;
    auto stream = analyzer.token_stream("f", text);
    EXPECT_TRUE(stream.has_value()) << (stream.has_value() ? "" : stream.error().to_string());
    if (!stream.has_value()) {
        return tokens;
    }
    EXPECT_TRUE(stream.value()->reset().has_value());
    while (true) {
        auto more = stream.value()->increment_token();
        EXPECT_TRUE(more.has_value());
        if (!more.has_value() || !more.value()) {
            break;
        }
        tokens.emplace_back(stream.value()->attributes().get_attribute<CharTermAttribute>()->text());
    }
    return tokens;
}

/// Runs `filter` over the whitespace tokens of `text`.  `source` is the tokenizer
/// the filter wraps, so the text can be fed in.
std::vector<std::string> run_filter(TokenStream &filter, Tokenizer &source, std::string_view text) {
    EXPECT_TRUE(source.set_reader(text).has_value());
    EXPECT_TRUE(filter.reset().has_value());
    std::vector<std::string> tokens;
    while (true) {
        auto more = filter.increment_token();
        EXPECT_TRUE(more.has_value());
        if (!more.has_value() || !more.value()) {
            break;
        }
        tokens.emplace_back(filter.attributes().get_attribute<CharTermAttribute>()->text());
    }
    return tokens;
}

} // namespace

TEST(LengthFilterTest, KeepsOnlyTokensInsideTheRange) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    LengthFilter filter(std::move(tokenizer), 3, 5);

    EXPECT_TRUE(source->set_reader("a ab abc abcde abcdef").has_value());
    EXPECT_TRUE(filter.reset().has_value());

    std::vector<std::string> tokens;
    std::vector<uint32_t> positions;
    while (true) {
        auto more = filter.increment_token();
        ASSERT_TRUE(more.has_value());
        if (!more.value()) {
            break;
        }
        tokens.emplace_back(filter.attributes().get_attribute<CharTermAttribute>()->text());
        positions.push_back(filter.attributes().get_attribute<PositionIncrementAttribute>()->position_increment());
    }
    EXPECT_EQ(tokens, (std::vector<std::string>{"abc", "abcde"}));
    // `a` and `ab` were dropped before `abc`, so its increment grew by two; the
    // two kept tokens stay adjacent.
    EXPECT_EQ(positions, (std::vector<uint32_t>{3, 1}));
}

TEST(TruncateTokenFilterTest, CutsAtCodePoints) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    TruncateTokenFilter filter(std::move(tokenizer), 3);
    // Lengths are counted in code points, so a three code point token survives.
    EXPECT_EQ(run_filter(filter, *source, "abcdef gh \xC3\xA9t\xC3\xA9\xC3\xA9"),
              (std::vector<std::string>{"abc", "gh", "\xC3\xA9t\xC3\xA9"}));
}

TEST(LimitTokenCountFilterTest, StopsAfterTheLimit) {
    {
        auto tokenizer = std::make_unique<WhitespaceTokenizer>();
        auto *source = tokenizer.get();
        LimitTokenCountFilter filter(std::move(tokenizer), 2);
        EXPECT_EQ(run_filter(filter, *source, "a b c d"), (std::vector<std::string>{"a", "b"}));
    }
    {
        // `consume_all_tokens` only changes the cost, not the tokens.
        auto tokenizer = std::make_unique<WhitespaceTokenizer>();
        auto *source = tokenizer.get();
        LimitTokenCountFilter filter(std::move(tokenizer), 2, true);
        EXPECT_EQ(run_filter(filter, *source, "a b c d"), (std::vector<std::string>{"a", "b"}));
    }
}

TEST(ElisionFilterTest, StripsTheArticleAndItsApostrophe) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    ElisionFilter filter(std::move(tokenizer), ElisionFilter::french_articles());
    // The ASCII apostrophe and U+2019 both elide; `les` keeps its form.
    EXPECT_EQ(run_filter(filter, *source, "l'avion l\xE2\x80\x99homme les amis"),
              (std::vector<std::string>{"avion", "homme", "les", "amis"}));
}

TEST(SetKeywordMarkerFilterTest, ProtectsTheMarkedTokensFromStemming) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    std::unique_ptr<TokenStream> chain = std::make_unique<SetKeywordMarkerFilter>(
            std::move(tokenizer), std::set<std::string, std::less<>>{"running"});
    auto *marked = chain.get();
    std::unique_ptr<TokenStream> stemmed = std::make_unique<PorterStemFilter>(std::move(chain));
    (void)marked;
    EXPECT_EQ(run_filter(*stemmed, *source, "running runners"), (std::vector<std::string>{"running", "runner"}));
}

TEST(EnglishAnalyzerTest, HonoursAStemExclusionSet) {
    EnglishAnalyzer analyzer(StopFilter::english_stop_words(), std::set<std::string, std::less<>>{"running"});

    // `running` is protected, `runners` is not, and `the` is a stop word.
    EXPECT_EQ(analyze(analyzer, "the running runners"), (std::vector<std::string>{"running", "runner"}));
}
