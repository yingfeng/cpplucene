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
#include <set>
#include <string>
#include <vector>

module lucene_core:ut.query_parser;

import lucene_core;

using namespace lucene;

namespace {

std::vector<int32_t> kinds(const std::vector<QueryToken> &tokens) {
    std::vector<int32_t> result;
    result.reserve(tokens.size());
    for (const QueryToken &token : tokens) {
        result.push_back(token.kind);
    }
    return result;
}

std::string bytes_to_string(const BytesRef &bytes) {
    const auto slice = bytes.as_slice();
    return std::string(reinterpret_cast<const char *>(slice.data()), slice.size());
}

std::shared_ptr<Query> parse_or_fail(QueryParser &parser, std::string_view text) {
    auto parsed = parser.parse(text);
    EXPECT_TRUE(parsed.has_value()) << (parsed.has_value() ? std::string() : parsed.error().to_string());
    return parsed.has_value() ? parsed.value() : nullptr;
}

std::string parse_error(QueryParser &parser, std::string_view text) {
    auto parsed = parser.parse(text);
    if (parsed.has_value()) {
        return {};
    }
    return parsed.error().to_string();
}

const Term &term_of(const std::shared_ptr<Query> &query) {
    static const Term empty;
    const auto *term_query = dynamic_cast<const TermQuery *>(query.get());
    return term_query != nullptr ? term_query->term() : empty;
}

} // namespace

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

TEST(QueryParserTokenTest, TokenizesTheClassicSyntax) {
    auto tokens = tokenize_query("title:foo AND bar* -\"a b\"~2");
    ASSERT_TRUE(tokens.has_value()) << tokens.error().to_string();
    EXPECT_EQ(tokens.value().size(), 9u);
    EXPECT_EQ(kinds(tokens.value()),
              (std::vector<int32_t>{query_syntax::TERM, query_syntax::COLON, query_syntax::TERM, query_syntax::AND,
                                    query_syntax::PREFIXTERM, query_syntax::MINUS, query_syntax::QUOTED,
                                    query_syntax::FUZZY_SLOP, query_syntax::EOF_TOKEN}));
    EXPECT_EQ(tokens.value()[0].image, "title");
    EXPECT_EQ(tokens.value()[4].image, "bar*");
    EXPECT_EQ(tokens.value()[6].image, "\"a b\"");
    EXPECT_EQ(tokens.value()[7].image, "~2");
}

TEST(QueryParserTokenTest, PrefersTheLongestMatchAndThenTheEarliestRule) {
    const auto kind_of = [](std::string_view text) {
        auto tokens = tokenize_query(text);
        EXPECT_TRUE(tokens.has_value()) << (tokens.has_value() ? std::string() : tokens.error().to_string());
        return tokens.has_value() ? tokens.value().front().kind : query_syntax::EOF_TOKEN;
    };

    // `AND` is an operator, `ANDR` a term, and the longer of two rules wins.
    EXPECT_EQ(kind_of("AND"), query_syntax::AND);
    EXPECT_EQ(kind_of("ANDR"), query_syntax::TERM);
    EXPECT_EQ(kind_of("&&"), query_syntax::AND);
    EXPECT_EQ(kind_of("NOT"), query_syntax::NOT);
    EXPECT_EQ(kind_of("NOTHING"), query_syntax::TERM);

    // `ab*` is a prefix term, `a*b` a wildcard term, a lone `*` the star token.
    EXPECT_EQ(kind_of("ab*"), query_syntax::PREFIXTERM);
    EXPECT_EQ(kind_of("a*b"), query_syntax::WILDTERM);
    EXPECT_EQ(kind_of("a?b"), query_syntax::WILDTERM);
    EXPECT_EQ(kind_of("*"), query_syntax::STAR);
    EXPECT_EQ(kind_of("a.b"), query_syntax::TERM);

    // The bare operator keeps its trailing blank, so `+ foo` and `+foo` differ.
    EXPECT_EQ(kind_of("+ foo"), query_syntax::BAREOPER);
    EXPECT_EQ(kind_of("+foo"), query_syntax::PLUS);
    EXPECT_EQ(kind_of("- foo"), query_syntax::BAREOPER);

    EXPECT_EQ(kind_of("/ab.*/"), query_syntax::REGEXPTERM);
    EXPECT_EQ(kind_of("\"a b\""), query_syntax::QUOTED);
    EXPECT_EQ(kind_of("[a TO b]"), query_syntax::RANGEIN_START);
    EXPECT_EQ(kind_of("{a TO b}"), query_syntax::RANGEEX_START);
    // `foo~0.8` is two tokens: the term and its slop.
    EXPECT_EQ(kind_of("~0.8"), query_syntax::FUZZY_SLOP);
}

TEST(QueryParserTokenTest, TokenizesRangeAndBoostStates) {
    auto range = tokenize_query("[a TO b]");
    ASSERT_TRUE(range.has_value()) << range.error().to_string();
    EXPECT_EQ(kinds(range.value()),
              (std::vector<int32_t>{query_syntax::RANGEIN_START, query_syntax::RANGE_GOOP, query_syntax::RANGE_TO,
                                    query_syntax::RANGE_GOOP, query_syntax::RANGEIN_END, query_syntax::EOF_TOKEN}));

    auto quoted_range = tokenize_query("{\"a b\" TO c}");
    ASSERT_TRUE(quoted_range.has_value()) << quoted_range.error().to_string();
    EXPECT_EQ(kinds(quoted_range.value()),
              (std::vector<int32_t>{query_syntax::RANGEEX_START, query_syntax::RANGE_QUOTED, query_syntax::RANGE_TO,
                                    query_syntax::RANGE_GOOP, query_syntax::RANGEEX_END, query_syntax::EOF_TOKEN}));

    auto boosted = tokenize_query("foo^2.5");
    ASSERT_TRUE(boosted.has_value()) << boosted.error().to_string();
    EXPECT_EQ(kinds(boosted.value()),
              (std::vector<int32_t>{query_syntax::TERM, query_syntax::CARAT, query_syntax::NUMBER,
                                    query_syntax::EOF_TOKEN}));
    EXPECT_EQ(boosted.value()[2].image, "2.5");

    // A boost that is not a number is a lexical error.
    auto broken = tokenize_query("foo^bar");
    EXPECT_FALSE(broken.has_value());
}

TEST(QueryParserTokenTest, ReportsThePositionOfAnUnexpectedCharacter) {
    auto tokens = tokenize_query("foo\n  ]bar");
    ASSERT_FALSE(tokens.has_value());
    EXPECT_NE(tokens.error().to_string().find("line 2, column 3"), std::string::npos);
}

TEST(QueryParserTokenTest, DiscardsEscapes) {
    auto escaped = discard_escape_char("a\\:b");
    ASSERT_TRUE(escaped.has_value());
    EXPECT_EQ(escaped.value(), "a:b");

    auto unicode = discard_escape_char("\\u0041BC");
    ASSERT_TRUE(unicode.has_value());
    EXPECT_EQ(unicode.value(), "ABC");

    auto chinese = discard_escape_char("\\u4e2d\\u6587");
    ASSERT_TRUE(chinese.has_value());
    EXPECT_EQ(chinese.value(), "中文");

    EXPECT_FALSE(discard_escape_char("abc\\").has_value());
    EXPECT_FALSE(discard_escape_char("a\\u12").has_value());
    EXPECT_FALSE(discard_escape_char("a\\u12zz").has_value());
}

// ---------------------------------------------------------------------------
// Parser: single clauses
// ---------------------------------------------------------------------------

TEST(QueryParserTest, ParsesTermsAndFields) {
    WhitespaceAnalyzer analyzer;
    QueryParser parser("body", analyzer);

    auto plain = parse_or_fail(parser, "foo");
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(term_of(plain).field(), "body");
    EXPECT_EQ(term_of(plain).text(), "foo");

    auto fielded = parse_or_fail(parser, "title:foo");
    ASSERT_NE(fielded, nullptr);
    EXPECT_EQ(term_of(fielded).field(), "title");
    EXPECT_EQ(term_of(fielded).text(), "foo");

    // An escaped colon belongs to the term.
    auto escaped = parse_or_fail(parser, "title\\:foo");
    ASSERT_NE(escaped, nullptr);
    EXPECT_EQ(term_of(escaped).field(), "body");
    EXPECT_EQ(term_of(escaped).text(), "title:foo");
}

TEST(QueryParserTest, ParsesPhrases) {
    WhitespaceAnalyzer analyzer;
    QueryParser parser("body", analyzer);

    auto phrase = parse_or_fail(parser, "\"foo bar\"");
    const auto *built = dynamic_cast<const PhraseQuery *>(phrase.get());
    ASSERT_NE(built, nullptr);
    ASSERT_EQ(built->terms().size(), 2u);
    EXPECT_EQ(built->terms()[0].text(), "foo");
    EXPECT_EQ(built->terms()[1].text(), "bar");
    EXPECT_EQ(built->positions(), (std::vector<int32_t>{0, 1}));
    EXPECT_EQ(built->slop(), 0);

    auto sloppy = parse_or_fail(parser, "\"foo bar\"~3");
    const auto *sloppy_phrase = dynamic_cast<const PhraseQuery *>(sloppy.get());
    ASSERT_NE(sloppy_phrase, nullptr);
    EXPECT_EQ(sloppy_phrase->slop(), 3);

    // A quoted single token stays a term query.
    auto single = parse_or_fail(parser, "\"foo\"");
    EXPECT_NE(dynamic_cast<const TermQuery *>(single.get()), nullptr);
}

TEST(QueryParserTest, ParsesPrefixWildcardAndRegexp) {
    WhitespaceAnalyzer analyzer;
    QueryParser parser("body", analyzer);

    auto prefix = parse_or_fail(parser, "foo*");
    const auto *prefix_query = dynamic_cast<const PrefixQuery *>(prefix.get());
    ASSERT_NE(prefix_query, nullptr);
    EXPECT_EQ(prefix_query->prefix(), "foo");

    auto wildcard = parse_or_fail(parser, "f?o*");
    const auto *wildcard_query = dynamic_cast<const WildcardQuery *>(wildcard.get());
    ASSERT_NE(wildcard_query, nullptr);
    EXPECT_EQ(wildcard_query->term().text(), "f?o*");

    auto regexp = parse_or_fail(parser, "/fo.*/");
    const auto *regexp_query = dynamic_cast<const RegexpQuery *>(regexp.get());
    ASSERT_NE(regexp_query, nullptr);
    EXPECT_EQ(regexp_query->term().text(), "fo.*");
}

TEST(QueryParserTest, RejectsALeadingWildcardUnlessAllowed) {
    WhitespaceAnalyzer analyzer;
    QueryParser parser("body", analyzer);

    EXPECT_NE(parse_error(parser, "*foo").find("not allowed as first character"), std::string::npos);
    EXPECT_NE(parse_error(parser, "?foo").find("not allowed as first character"), std::string::npos);

    parser.set_allow_leading_wildcard(true);
    auto allowed = parse_or_fail(parser, "*foo");
    const auto *wildcard_query = dynamic_cast<const WildcardQuery *>(allowed.get());
    ASSERT_NE(wildcard_query, nullptr);
    EXPECT_EQ(wildcard_query->term().text(), "*foo");
}

TEST(QueryParserTest, ParsesFuzzyTerms) {
    WhitespaceAnalyzer analyzer;
    QueryParser parser("body", analyzer);

    // The default similarity is 0.5, which for a three character term is one edit.
    auto fuzzy = parse_or_fail(parser, "foo~");
    const auto *fuzzy_query = dynamic_cast<const FuzzyQuery *>(fuzzy.get());
    ASSERT_NE(fuzzy_query, nullptr);
    EXPECT_EQ(fuzzy_query->max_edits(), 1);
    EXPECT_EQ(fuzzy_query->term().text(), "foo");

    auto exact = parse_or_fail(parser, "foo~1.0");
    const auto *exact_query = dynamic_cast<const FuzzyQuery *>(exact.get());
    ASSERT_NE(exact_query, nullptr);
    EXPECT_EQ(exact_query->max_edits(), FuzzyQuery::float_to_edits(1.0F, 3));

    auto two_edits = parse_or_fail(parser, "foo~2");
    const auto *two_edit_query = dynamic_cast<const FuzzyQuery *>(two_edits.get());
    ASSERT_NE(two_edit_query, nullptr);
    EXPECT_EQ(two_edit_query->max_edits(), 2);
}

TEST(QueryParserTest, ParsesRanges) {
    WhitespaceAnalyzer analyzer;
    QueryParser parser("body", analyzer);

    auto inclusive = parse_or_fail(parser, "[a TO c]");
    const auto *inclusive_query = dynamic_cast<const TermRangeQuery *>(inclusive.get());
    ASSERT_NE(inclusive_query, nullptr);
    ASSERT_TRUE(inclusive_query->lower_term().has_value());
    ASSERT_TRUE(inclusive_query->upper_term().has_value());
    EXPECT_EQ(bytes_to_string(inclusive_query->lower_term().value()), "a");
    EXPECT_EQ(bytes_to_string(inclusive_query->upper_term().value()), "c");
    EXPECT_TRUE(inclusive_query->includes_lower());
    EXPECT_TRUE(inclusive_query->includes_upper());

    auto exclusive = parse_or_fail(parser, "{a TO c}");
    const auto *exclusive_query = dynamic_cast<const TermRangeQuery *>(exclusive.get());
    ASSERT_NE(exclusive_query, nullptr);
    EXPECT_FALSE(exclusive_query->includes_lower());
    EXPECT_FALSE(exclusive_query->includes_upper());

    auto open_lower = parse_or_fail(parser, "[* TO c]");
    const auto *open_lower_query = dynamic_cast<const TermRangeQuery *>(open_lower.get());
    ASSERT_NE(open_lower_query, nullptr);
    EXPECT_FALSE(open_lower_query->lower_term().has_value());
    ASSERT_TRUE(open_lower_query->upper_term().has_value());

    auto open_upper = parse_or_fail(parser, "date:{2020 TO *}");
    const auto *open_upper_query = dynamic_cast<const TermRangeQuery *>(open_upper.get());
    ASSERT_NE(open_upper_query, nullptr);
    EXPECT_EQ(open_upper_query->term().field(), "date");
    EXPECT_FALSE(open_upper_query->upper_term().has_value());
}

TEST(QueryParserTest, AppliesBoosts) {
    WhitespaceAnalyzer analyzer;
    QueryParser parser("body", analyzer);

    auto boosted = parse_or_fail(parser, "foo^2.5");
    const auto *boost_query = dynamic_cast<const BoostQuery *>(boosted.get());
    ASSERT_NE(boost_query, nullptr);
    EXPECT_FLOAT_EQ(boost_query->boost(), 2.5F);
    EXPECT_EQ(term_of(boost_query->query()).text(), "foo");

    auto nested = parse_or_fail(parser, "(a OR b)^3");
    const auto *nested_boost = dynamic_cast<const BoostQuery *>(nested.get());
    ASSERT_NE(nested_boost, nullptr);
    EXPECT_FLOAT_EQ(nested_boost->boost(), 3.0F);
    EXPECT_NE(dynamic_cast<const BooleanQuery *>(nested_boost->query().get()), nullptr);
}

// ---------------------------------------------------------------------------
// Parser: boolean structure
// ---------------------------------------------------------------------------

TEST(QueryParserTest, BuildsBooleanClausesFromOperators) {
    WhitespaceAnalyzer analyzer;
    QueryParser parser("body", analyzer);

    auto conjunctive = parse_or_fail(parser, "a AND b");
    const auto *and_query = dynamic_cast<const BooleanQuery *>(conjunctive.get());
    ASSERT_NE(and_query, nullptr);
    ASSERT_EQ(and_query->clauses().size(), 2u);
    EXPECT_EQ(and_query->clauses()[0].occur, Occur::Must);
    EXPECT_EQ(and_query->clauses()[1].occur, Occur::Must);

    auto disjunctive = parse_or_fail(parser, "a OR b");
    const auto *or_query = dynamic_cast<const BooleanQuery *>(disjunctive.get());
    ASSERT_NE(or_query, nullptr);
    ASSERT_EQ(or_query->clauses().size(), 2u);
    EXPECT_EQ(or_query->clauses()[0].occur, Occur::Should);
    EXPECT_EQ(or_query->clauses()[1].occur, Occur::Should);

    auto prohibited = parse_or_fail(parser, "a NOT b");
    const auto *not_query = dynamic_cast<const BooleanQuery *>(prohibited.get());
    ASSERT_NE(not_query, nullptr);
    ASSERT_EQ(not_query->clauses().size(), 2u);
    EXPECT_EQ(not_query->clauses()[1].occur, Occur::MustNot);

    auto signed_terms = parse_or_fail(parser, "+a -b");
    const auto *signed_query = dynamic_cast<const BooleanQuery *>(signed_terms.get());
    ASSERT_NE(signed_query, nullptr);
    ASSERT_EQ(signed_query->clauses().size(), 2u);
    EXPECT_EQ(signed_query->clauses()[0].occur, Occur::Must);
    EXPECT_EQ(signed_query->clauses()[1].occur, Occur::MustNot);
}

TEST(QueryParserTest, HonoursTheDefaultOperator) {
    WhitespaceAnalyzer analyzer;
    QueryParser or_parser("body", analyzer);
    auto disjunctive = parse_or_fail(or_parser, "a b");
    const auto *or_query = dynamic_cast<const BooleanQuery *>(disjunctive.get());
    ASSERT_NE(or_query, nullptr);
    ASSERT_EQ(or_query->clauses().size(), 2u);
    EXPECT_EQ(or_query->clauses()[0].occur, Occur::Should);

    QueryParser and_parser("body", analyzer);
    and_parser.set_default_operator(QueryParserOperator::kAnd);
    auto conjunctive = parse_or_fail(and_parser, "a b");
    const auto *and_query = dynamic_cast<const BooleanQuery *>(conjunctive.get());
    ASSERT_NE(and_query, nullptr);
    ASSERT_EQ(and_query->clauses().size(), 2u);
    EXPECT_EQ(and_query->clauses()[0].occur, Occur::Must);
}

TEST(QueryParserTest, MergesWhitespaceRunsForTheAnalyzer) {
    WhitespaceAnalyzer analyzer;
    QueryParser parser("body", analyzer);

    // `split_on_whitespace` is off by default, so `foo bar` reaches the analyzer
    // as one text and comes back as two positions of one clause.
    auto merged = parse_or_fail(parser, "foo bar");
    const auto *merged_query = dynamic_cast<const BooleanQuery *>(merged.get());
    ASSERT_NE(merged_query, nullptr);
    ASSERT_EQ(merged_query->clauses().size(), 2u);
    EXPECT_EQ(merged_query->clauses()[0].occur, Occur::Should);

    // A field applies to the first term only; the rest keeps the default field.
    auto mixed = parse_or_fail(parser, "title:foo bar");
    const auto *mixed_query = dynamic_cast<const BooleanQuery *>(mixed.get());
    ASSERT_NE(mixed_query, nullptr);
    ASSERT_EQ(mixed_query->clauses().size(), 2u);
    EXPECT_EQ(term_of(mixed_query->clauses()[0].query).field(), "title");
    EXPECT_EQ(term_of(mixed_query->clauses()[1].query).field(), "body");

    // With `split_on_whitespace` on, each word is a clause of its own.
    QueryParser splitting("body", analyzer);
    ASSERT_TRUE(splitting.set_split_on_whitespace(true).has_value());
    auto split = parse_or_fail(splitting, "a AND b");
    const auto *split_query = dynamic_cast<const BooleanQuery *>(split.get());
    ASSERT_NE(split_query, nullptr);
    EXPECT_EQ(split_query->clauses().size(), 2u);
}

TEST(QueryParserTest, ParsesNestedQueries) {
    WhitespaceAnalyzer analyzer;
    QueryParser parser("body", analyzer);

    auto nested = parse_or_fail(parser, "(a AND (b OR c))");
    const auto *outer = dynamic_cast<const BooleanQuery *>(nested.get());
    ASSERT_NE(outer, nullptr);
    ASSERT_EQ(outer->clauses().size(), 2u);
    const auto *inner = dynamic_cast<const BooleanQuery *>(outer->clauses()[1].query.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->clauses().size(), 2u);
}

TEST(QueryParserTest, RejectsMalformedQueries) {
    WhitespaceAnalyzer analyzer;
    QueryParser parser("body", analyzer);

    EXPECT_FALSE(parse_error(parser, "(a").empty());
    EXPECT_FALSE(parse_error(parser, "a)").empty());
    EXPECT_FALSE(parse_error(parser, "a:").empty());
    EXPECT_FALSE(parse_error(parser, "[a b]").empty());
    EXPECT_FALSE(parse_error(parser, "foo~0.5~").empty());
}

TEST(QueryParserTest, DropsQueriesThatAnalyzeAway) {
    StandardAnalyzer analyzer(std::set<std::string, std::less<>>{"the"});
    QueryParser parser("body", analyzer);

    auto filtered = parser.parse("the");
    ASSERT_TRUE(filtered.has_value()) << filtered.error().to_string();
    EXPECT_EQ(filtered.value(), nullptr);

    auto kept = parse_or_fail(parser, "the foo");
    ASSERT_NE(kept, nullptr);
    EXPECT_EQ(term_of(kept).text(), "foo");
}

TEST(QueryParserTest, EscapesTheSyntaxCharacters) {
    EXPECT_EQ(QueryParserBase::escape("a:b"), "a\\:b");
    EXPECT_EQ(QueryParserBase::escape("(a)"), "\\(a\\)");
    EXPECT_EQ(QueryParserBase::escape("plain"), "plain");

    WhitespaceAnalyzer analyzer;
    QueryParser parser("body", analyzer);
    auto escaped = parse_or_fail(parser, QueryParserBase::escape("a:b"));
    EXPECT_EQ(term_of(escaped).text(), "a:b");
}

// ---------------------------------------------------------------------------
// QueryBuilder
// ---------------------------------------------------------------------------

TEST(QueryBuilderTest, BuildsPhrasesAndBooleans) {
    WhitespaceAnalyzer analyzer;
    QueryBuilder builder(analyzer);

    auto term = builder.create_boolean_query("body", "foo");
    ASSERT_TRUE(term.has_value()) << term.error().to_string();
    EXPECT_EQ(term_of(term.value()).text(), "foo");

    auto conjunction = builder.create_boolean_query("body", "foo bar");
    ASSERT_TRUE(conjunction.has_value()) << conjunction.error().to_string();
    const auto *boolean = dynamic_cast<const BooleanQuery *>(conjunction.value().get());
    ASSERT_NE(boolean, nullptr);
    ASSERT_EQ(boolean->clauses().size(), 2u);
    EXPECT_EQ(boolean->clauses()[0].occur, Occur::Should);

    auto required = builder.create_boolean_query("body", "foo bar", Occur::Must);
    ASSERT_TRUE(required.has_value()) << required.error().to_string();
    const auto *required_boolean = dynamic_cast<const BooleanQuery *>(required.value().get());
    ASSERT_NE(required_boolean, nullptr);
    EXPECT_EQ(required_boolean->clauses()[0].occur, Occur::Must);

    auto phrase = builder.create_phrase_query("body", "foo bar");
    ASSERT_TRUE(phrase.has_value()) << phrase.error().to_string();
    const auto *phrase_query = dynamic_cast<const PhraseQuery *>(phrase.value().get());
    ASSERT_NE(phrase_query, nullptr);
    EXPECT_EQ(phrase_query->terms().size(), 2u);

    auto sloppy = builder.create_phrase_query("body", "foo bar", 4);
    ASSERT_TRUE(sloppy.has_value()) << sloppy.error().to_string();
    const auto *sloppy_query = dynamic_cast<const PhraseQuery *>(sloppy.value().get());
    ASSERT_NE(sloppy_query, nullptr);
    EXPECT_EQ(sloppy_query->slop(), 4);

    EXPECT_FALSE(builder.create_boolean_query("body", "", Occur::Filter).has_value());
}

TEST(QueryBuilderTest, EmptyAnalysisProducesNoQuery) {
    WhitespaceAnalyzer analyzer;
    QueryBuilder builder(analyzer);

    auto empty = builder.create_boolean_query("body", "");
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty.value(), nullptr);

    auto spaces = builder.create_phrase_query("body", "   ");
    ASSERT_TRUE(spaces.has_value());
    EXPECT_EQ(spaces.value(), nullptr);
}
