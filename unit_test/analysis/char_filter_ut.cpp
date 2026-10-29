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

#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.char_filter;

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

} // namespace

TEST(MappingCharFilterTest, ReplacesTheLongestRuleAtEachPosition) {
    MappingCharFilter filter({{"a", "1"}, {"ab", "2"}, {"b", "3"}});
    auto filtered = filter.filter("abc");
    ASSERT_TRUE(filtered.has_value()) << (filtered.has_value() ? "" : filtered.error().to_string());
    // `ab` wins over `a`, then `c` has no rule and is kept.
    EXPECT_EQ(filtered.value(), "2c");

    MappingCharFilter empty_rule({{"x", ""}});
    auto deleted = empty_rule.filter("axb");
    ASSERT_TRUE(deleted.has_value());
    EXPECT_EQ(deleted.value(), "ab");
}

TEST(MappingCharFilterTest, ParsesMappingRules) {
    const std::string rules = "# a comment\n"
                              "\"\xC3\x80\" => \"A\"\n"
                              "\n"
                              "\"\xC3\x89\" => \"E\"\n"
                              "\"\\u00e7\" => \"c\"\n";
    auto parsed = MappingCharFilter::parse(rules);
    ASSERT_TRUE(parsed.has_value()) << (parsed.has_value() ? "" : parsed.error().to_string());
    EXPECT_EQ(parsed.value().mappings().size(), 3u);

    auto filtered = parsed.value().filter("\xC3\x80\xC3\x89\xC3\xA7");
    ASSERT_TRUE(filtered.has_value());
    EXPECT_EQ(filtered.value(), "AEc");
}

TEST(MappingCharFilterTest, RejectsMalformedRules) {
    EXPECT_FALSE(MappingCharFilter::parse("\"a\" -> \"b\"").has_value());
    EXPECT_FALSE(MappingCharFilter::parse("\"a\" => \"b").has_value());
    EXPECT_FALSE(MappingCharFilter::parse("a => b").has_value());
}

TEST(PatternReplaceCharFilterTest, ReplacesWithTheCaptureGroups) {
    auto filter = PatternReplaceCharFilter::create("(\\d{3})-(\\d{4})", "$1$2");
    ASSERT_TRUE(filter.has_value()) << (filter.has_value() ? "" : filter.error().to_string());
    auto filtered = filter.value().filter("call 555-1234 now");
    ASSERT_TRUE(filtered.has_value());
    EXPECT_EQ(filtered.value(), "call 5551234 now");

    EXPECT_FALSE(PatternReplaceCharFilter::create("(", "x").has_value());
}

TEST(HTMLStripCharFilterTest, DropsMarkupAndDecodesEntities) {
    HTMLStripCharFilter filter;

    auto stripped = filter.filter("<b>Hello</b> &amp; welcome");
    ASSERT_TRUE(stripped.has_value()) << (stripped.has_value() ? "" : stripped.error().to_string());
    EXPECT_EQ(stripped.value(), " Hello  & welcome");

    // Numeric entities, decimal and hexadecimal.
    auto numeric = filter.filter("&#233;t&#xE9;");
    ASSERT_TRUE(numeric.has_value());
    EXPECT_EQ(numeric.value(), "été");

    // The content of `script` and `style` disappears with the tag.
    auto script = filter.filter("<p>hi</p><script>var a = 1 < 2;</script><style>b{}</style><p>there</p>");
    ASSERT_TRUE(script.has_value());
    // Each tag becomes a space, and the `script`/`style` elements add one more
    // for their dropped content.
    EXPECT_EQ(script.value(), " hi    there ");

    // An unknown entity stays as it is.
    auto unknown = filter.filter("a &nosuch; b");
    ASSERT_TRUE(unknown.has_value());
    EXPECT_EQ(unknown.value(), "a &nosuch; b");
}

TEST(CharFilterAnalyzerTest, FeedsTheTransformedTextToTheWrappedAnalyzer) {
    std::vector<std::unique_ptr<CharFilter>> filters;
    filters.push_back(std::make_unique<HTMLStripCharFilter>());
    CharFilterAnalyzer analyzer(std::make_unique<WhitespaceAnalyzer>(), std::move(filters));

    EXPECT_EQ(analyze(analyzer, "<p>Hello</p> &amp; <b>welcome</b>"),
              (std::vector<std::string>{"Hello", "&", "welcome"}));
    EXPECT_EQ(analyzer.to_string(), "CharFilterAnalyzer(HTMLStripCharFilter, WhitespaceAnalyzer)");
}

TEST(CharFilterAnalyzerTest, ChainsSeveralFilters) {
    std::vector<std::unique_ptr<CharFilter>> filters;
    filters.push_back(std::make_unique<HTMLStripCharFilter>());
    auto mapping = MappingCharFilter::parse("\"caf\xC3\xA9\" => \"cafe\"");
    ASSERT_TRUE(mapping.has_value());
    filters.push_back(std::make_unique<MappingCharFilter>(std::move(mapping.value())));

    CharFilterAnalyzer analyzer(std::make_unique<WhitespaceAnalyzer>(), std::move(filters));
    EXPECT_EQ(analyze(analyzer, "<i>caf\xC3\xA9</i>"), (std::vector<std::string>{"cafe"}));
}
