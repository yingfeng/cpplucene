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
#include <vector>

module lucene_core:ut.text_analysis_filters;

import lucene_core;

using namespace lucene;

namespace {

std::vector<std::string> analyze(Analyzer &analyzer, std::string_view text) {
    std::vector<std::string> tokens;
    auto stream = analyzer.token_stream("f", text);
    EXPECT_TRUE(stream.has_value()) << (stream.has_value() ? std::string() : stream.error().to_string());
    if (!stream.has_value()) {
        return tokens;
    }
    TokenStream *token_stream = stream.value();
    EXPECT_TRUE(token_stream->reset().has_value());
    while (true) {
        auto more = token_stream->increment_token();
        EXPECT_TRUE(more.has_value());
        if (!more.has_value() || !more.value()) {
            break;
        }
        auto *term = token_stream->attributes().get_attribute<CharTermAttribute>();
        tokens.emplace_back(term->text());
    }
    EXPECT_TRUE(token_stream->end().has_value());
    EXPECT_TRUE(token_stream->close().has_value());
    return tokens;
}

} // namespace

// ---------------------------------------------------------------------------
// Porter stemmer
// ---------------------------------------------------------------------------

TEST(PorterStemmerTest, StemsThePublishedVocabulary) {
    struct Case {
        std::string_view word;
        std::string_view stem;
    };
    const std::vector<Case> cases{
            // step 1: plurals and -ed/-ing
            {"caresses", "caress"}, {"ponies", "poni"}, {"ties", "ti"}, {"caress", "caress"}, {"cats", "cat"},
            {"feed", "feed"}, {"agreed", "agre"}, {"plastered", "plaster"}, {"bled", "bled"}, {"motoring", "motor"},
            {"sing", "sing"}, {"conflated", "conflat"}, {"troubled", "troubl"}, {"sized", "size"}, {"hopping", "hop"},
            {"tanned", "tan"}, {"falling", "fall"}, {"hissing", "hiss"}, {"fizzed", "fizz"}, {"failing", "fail"},
            {"filing", "file"},
            // step 2: terminal y
            {"happy", "happi"}, {"sky", "sky"},
            // step 3: -ational, -ization, -ness, ...
            {"relational", "relat"}, {"conditional", "condit"}, {"rational", "ration"}, {"valenci", "valenc"},
            {"hesitanci", "hesit"}, {"digitizer", "digit"}, {"conformabli", "conform"}, {"radicalli", "radic"},
            {"differentli", "differ"}, {"vileli", "vile"}, {"analogousli", "analog"}, {"vietnamization", "vietnam"},
            {"predication", "predic"}, {"operator", "oper"}, {"feudalism", "feudal"}, {"decisiveness", "decis"},
            {"hopefulness", "hope"}, {"callousness", "callous"}, {"formaliti", "formal"}, {"sensitiviti", "sensit"},
            {"sensibiliti", "sensibl"},
            // step 4: -ic-, -ful, -ness
            {"triplicate", "triplic"}, {"formative", "form"}, {"formalize", "formal"}, {"electriciti", "electr"},
            {"electrical", "electr"}, {"hopeful", "hope"}, {"goodness", "good"},
            // step 5: -ant, -ence, -ion, ...
            {"revival", "reviv"}, {"allowance", "allow"}, {"inference", "infer"}, {"airliner", "airlin"},
            {"gyroscopic", "gyroscop"}, {"adjustable", "adjust"}, {"defensible", "defens"}, {"irritant", "irrit"},
            {"replacement", "replac"}, {"adjustment", "adjust"}, {"dependent", "depend"}, {"adoption", "adopt"},
            {"communism", "commun"}, {"activate", "activ"}, {"angulariti", "angular"}, {"homologou", "homolog"},
            {"effective", "effect"}, {"bowdlerize", "bowdler"},
            // step 6: final -e
            {"probate", "probat"}, {"rate", "rate"}, {"cease", "ceas"}, {"controll", "control"}, {"roll", "roll"},
    };

    PorterStemmer stemmer;
    for (const Case &current : cases) {
        EXPECT_EQ(stemmer.stem(current.word), current.stem) << "word: " << current.word;
    }
}

TEST(PorterStemmerTest, LeavesShortAndNonAsciiTermsAlone) {
    PorterStemmer stemmer;
    EXPECT_EQ(stemmer.stem(""), "");
    EXPECT_EQ(stemmer.stem("a"), "a");
    EXPECT_EQ(stemmer.stem("as"), "as");
    EXPECT_EQ(stemmer.stem("be"), "be");
    EXPECT_EQ(stemmer.stem("中文"), "中文");
    EXPECT_EQ(stemmer.stem("日本語"), "日本語");
    EXPECT_EQ(stemmer.stem("café"), "café");
}

// ---------------------------------------------------------------------------
// English chain
// ---------------------------------------------------------------------------

TEST(EnglishAnalyzerTest, StemsAndRemovesStopWords) {
    EnglishAnalyzer analyzer;
    EXPECT_EQ(analyze(analyzer, "The quick brown foxes are running"),
              (std::vector<std::string>{"quick", "brown", "fox", "run"}));
}

TEST(EnglishAnalyzerTest, StripsThePossessive) {
    EnglishAnalyzer analyzer;
    EXPECT_EQ(analyze(analyzer, "John's books"), (std::vector<std::string>{"john", "book"}));
    EXPECT_EQ(analyze(analyzer, "students' notes"), (std::vector<std::string>{"student", "note"}));
}

TEST(EnglishAnalyzerTest, HonoursACustomStopWordSet) {
    EnglishAnalyzer analyzer(std::set<std::string, std::less<>>{"quick"});
    EXPECT_EQ(analyze(analyzer, "the quick brown fox"), (std::vector<std::string>{"the", "brown", "fox"}));
}

// ---------------------------------------------------------------------------
// CJK chain
// ---------------------------------------------------------------------------

TEST(CJKWidthFilterTest, FoldsFullWidthAndHalfWidthCharacters) {
    EXPECT_EQ(CJKWidthFilter::fold("ＡＢＣ"), "ABC");
    EXPECT_EQ(CJKWidthFilter::fold("１２３"), "123");
    EXPECT_EQ(CJKWidthFilter::fold("\u3000"), " ");
    EXPECT_EQ(CJKWidthFilter::fold("ｱｲｳ"), "アイウ");
    EXPECT_EQ(CJKWidthFilter::fold("abc"), "abc");
    EXPECT_EQ(CJKWidthFilter::fold("中文"), "中文");
}

TEST(CJKAnalyzerTest, IndexesChineseAsBigrams) {
    CJKAnalyzer analyzer;
    EXPECT_EQ(analyze(analyzer, "中国人"), (std::vector<std::string>{"中国", "国人"}));
    EXPECT_EQ(analyze(analyzer, "中华人民共和国"),
              (std::vector<std::string>{"中华", "华人", "人民", "民共", "共和", "和国"}));
    // A single character stays a unigram.
    EXPECT_EQ(analyze(analyzer, "中"), (std::vector<std::string>{"中"}));
}

TEST(CJKAnalyzerTest, IndexesJapaneseAndKoreanAsBigrams) {
    CJKAnalyzer analyzer;
    EXPECT_EQ(analyze(analyzer, "日本語"), (std::vector<std::string>{"日本", "本語"}));
    EXPECT_EQ(analyze(analyzer, "한국어"), (std::vector<std::string>{"한국", "국어"}));
}

TEST(CJKAnalyzerTest, KeepsLatinTextApart) {
    CJKAnalyzer analyzer;
    // Latin words are not bigrammed, and a run of one CJK character stays whole.
    EXPECT_EQ(analyze(analyzer, "abc 中国"), (std::vector<std::string>{"abc", "中国"}));
    EXPECT_EQ(analyze(analyzer, "Machine 学习"), (std::vector<std::string>{"machine", "学习"}));
}

TEST(CJKAnalyzerTest, FoldsWidthBeforeBigramming) {
    CJKAnalyzer analyzer;
    // The full width letters fold to ASCII and stay one token.
    EXPECT_EQ(analyze(analyzer, "ＲＡＧ"), (std::vector<std::string>{"rag"}));
}

// ---------------------------------------------------------------------------
// N-gram tokenizer and filter
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> collect_stream(TokenStream &stream) {
    std::vector<std::string> tokens;
    EXPECT_TRUE(stream.reset().has_value());
    while (true) {
        auto more = stream.increment_token();
        EXPECT_TRUE(more.has_value());
        if (!more.has_value() || !more.value()) {
            break;
        }
        auto *term = stream.attributes().get_attribute<CharTermAttribute>();
        tokens.emplace_back(term->text());
    }
    return tokens;
}

std::vector<std::string> collect_grams(Tokenizer &tokenizer, std::string_view text) {
    EXPECT_TRUE(tokenizer.set_reader(text).has_value());
    return collect_stream(tokenizer);
}

} // namespace

TEST(NGramTokenizerTest, EmitsOffsetThenLengthOrder) {
    NGramTokenizer tokenizer(2, 3);
    EXPECT_EQ(collect_grams(tokenizer, "abcd"),
              (std::vector<std::string>{"ab", "abc", "bc", "bcd", "cd"}));

    NGramTokenizer single(3, 3);
    EXPECT_EQ(collect_grams(single, "abcd"), (std::vector<std::string>{"abc", "bcd"}));

    NGramTokenizer too_long(5, 5);
    EXPECT_TRUE(collect_grams(too_long, "abc").empty());
}

TEST(EdgeNGramTokenizerTest, EmitsOnlyGramsAtTheStart) {
    EdgeNGramTokenizer tokenizer(1, 3);
    EXPECT_EQ(collect_grams(tokenizer, "abcd"), (std::vector<std::string>{"a", "ab", "abc"}));
}

TEST(NGramTokenFilterTest, ExplodesEachTermAndSharesThePosition) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    NGramTokenFilter filter(std::move(tokenizer), 2, 2);

    EXPECT_TRUE(source->set_reader("abc xy").has_value());
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

    EXPECT_EQ(tokens, (std::vector<std::string>{"ab", "bc", "xy"}));
    EXPECT_EQ(positions, (std::vector<uint32_t>{1, 0, 1}));
}

TEST(NGramTokenFilterTest, PreserveOriginalKeepsShortAndLongTerms) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    NGramTokenFilter filter(std::move(tokenizer), 2, 3, true);

    EXPECT_TRUE(source->set_reader("a abcd").has_value());
    EXPECT_TRUE(filter.reset().has_value());

    std::vector<std::string> tokens;
    while (true) {
        auto more = filter.increment_token();
        ASSERT_TRUE(more.has_value());
        if (!more.value()) {
            break;
        }
        tokens.emplace_back(filter.attributes().get_attribute<CharTermAttribute>()->text());
    }

    // `a` is shorter than minGram and `abcd` longer than maxGram: both survive.
    EXPECT_EQ(tokens, (std::vector<std::string>{"a", "ab", "abc", "bc", "bcd", "cd", "abcd"}));
}

TEST(EdgeNGramTokenFilterTest, EmitsPrefixesOnly) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    EdgeNGramTokenFilter filter(std::move(tokenizer), 2, 3);

    EXPECT_TRUE(source->set_reader("abcd").has_value());
    EXPECT_TRUE(filter.reset().has_value());

    std::vector<std::string> tokens;
    while (true) {
        auto more = filter.increment_token();
        ASSERT_TRUE(more.has_value());
        if (!more.value()) {
            break;
        }
        tokens.emplace_back(filter.attributes().get_attribute<CharTermAttribute>()->text());
    }
    EXPECT_EQ(tokens, (std::vector<std::string>{"ab", "abc"}));
}

// ---------------------------------------------------------------------------
// Shingles
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> shingles(int32_t min_size, int32_t max_size, bool output_unigrams, std::string_view text,
                                 std::string_view separator = " ") {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    ShingleFilter filter(std::move(tokenizer), min_size, max_size, output_unigrams);
    filter.set_token_separator(separator);
    EXPECT_TRUE(source->set_reader(text).has_value());
    return collect_stream(filter);
}

} // namespace

TEST(ShingleFilterTest, EmitsEveryShingleAtThePositionOfItsFirstToken) {
    EXPECT_EQ(shingles(2, 2, true, "a b c"),
              (std::vector<std::string>{"a", "a b", "b", "b c", "c"}));
    EXPECT_EQ(shingles(2, 3, true, "a b c"),
              (std::vector<std::string>{"a", "a b", "a b c", "b", "b c", "c"}));
    // Without unigrams only the pairs remain.
    EXPECT_EQ(shingles(2, 2, false, "a b c"), (std::vector<std::string>{"a b", "b c"}));
    // The separator is configurable.
    EXPECT_EQ(shingles(2, 2, false, "a b c", "_"), (std::vector<std::string>{"a_b", "b_c"}));
    // A single token cannot form a bigram.
    EXPECT_EQ(shingles(2, 2, true, "only"), (std::vector<std::string>{"only"}));
}

// ---------------------------------------------------------------------------
// ASCII folding
// ---------------------------------------------------------------------------

TEST(ASCIIFoldingFilterTest, FoldsLatin1AndExtendedA) {
    EXPECT_EQ(ASCIIFoldingFilter::fold("café"), "cafe");
    EXPECT_EQ(ASCIIFoldingFilter::fold("Ångström"), "Angstrom");
    EXPECT_EQ(ASCIIFoldingFilter::fold("naïve"), "naive");
    EXPECT_EQ(ASCIIFoldingFilter::fold("Łódź"), "Lodz");
    EXPECT_EQ(ASCIIFoldingFilter::fold("straße"), "strasse");
    EXPECT_EQ(ASCIIFoldingFilter::fold("Æsir"), "AEsir");
    // Untouched: plain ASCII, multiplication sign, CJK.
    EXPECT_EQ(ASCIIFoldingFilter::fold("plain"), "plain");
    EXPECT_EQ(ASCIIFoldingFilter::fold("3×4"), "3×4");
    EXPECT_EQ(ASCIIFoldingFilter::fold("中文"), "中文");
}

TEST(ASCIIFoldingFilterTest, FoldsThroughTheTokenStream) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    ASCIIFoldingFilter filter(std::move(tokenizer));

    EXPECT_TRUE(source->set_reader("café résumé").has_value());
    EXPECT_EQ(collect_stream(filter), (std::vector<std::string>{"cafe", "resume"}));
}

TEST(ASCIIFoldingFilterTest, PreserveOriginalKeepsBothForms) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    ASCIIFoldingFilter filter(std::move(tokenizer), true);

    EXPECT_TRUE(source->set_reader("café plain").has_value());
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
    EXPECT_EQ(tokens, (std::vector<std::string>{"cafe", "café", "plain"}));
    EXPECT_EQ(positions, (std::vector<uint32_t>{1, 0, 1}));
}

// ---------------------------------------------------------------------------
// Synonyms
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> synonyms(const SynonymMap &map, std::string_view text, bool ignore_case = false) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    SynonymFilter filter(std::move(tokenizer), map, ignore_case);
    EXPECT_TRUE(source->set_reader(text).has_value());
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

std::vector<uint32_t> synonym_positions(const SynonymMap &map, std::string_view text) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    SynonymFilter filter(std::move(tokenizer), map);
    EXPECT_TRUE(source->set_reader(text).has_value());
    EXPECT_TRUE(filter.reset().has_value());

    std::vector<uint32_t> positions;
    while (true) {
        auto more = filter.increment_token();
        EXPECT_TRUE(more.has_value());
        if (!more.has_value() || !more.value()) {
            break;
        }
        positions.push_back(filter.attributes().get_attribute<PositionIncrementAttribute>()->position_increment());
    }
    return positions;
}

} // namespace

TEST(SynonymFilterTest, ExpandsSingleWordAlternativesAtOnePosition) {
    SynonymMap::Builder builder;
    builder.add({"fast"}, {"quick"}, false);
    builder.add({"fast"}, {"speedy"}, false);
    const SynonymMap map = builder.build();
    EXPECT_EQ(map.size(), 1u);

    // Both alternatives sit at the position of `fast`, and `car` keeps its own.
    EXPECT_EQ(synonyms(map, "fast car"), (std::vector<std::string>{"quick", "speedy", "car"}));
    EXPECT_EQ(synonym_positions(map, "fast car"), (std::vector<uint32_t>{1, 0, 1}));
}

TEST(SynonymFilterTest, KeepsTheOriginalWhenAsked) {
    SynonymMap::Builder builder;
    builder.add({"tv"}, {"television"}, true);
    const SynonymMap map = builder.build();

    const std::vector<std::string> tokens = synonyms(map, "tv");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_NE(std::find(tokens.begin(), tokens.end(), "tv"), tokens.end());
    EXPECT_NE(std::find(tokens.begin(), tokens.end(), "television"), tokens.end());
}

TEST(SynonymFilterTest, MatchesMultiWordRules) {
    SynonymMap::Builder builder;
    builder.add({"new", "york"}, {"ny"}, false);
    const SynonymMap map = builder.build();
    EXPECT_EQ(map.max_input_length(), 2u);

    EXPECT_EQ(synonyms(map, "new york city"), (std::vector<std::string>{"ny", "city"}));
    // A rule only fires when the whole sequence is present.
    EXPECT_EQ(synonyms(map, "new city"), (std::vector<std::string>{"new", "city"}));
}

TEST(SynonymFilterTest, IgnoresCaseWhenAsked) {
    SynonymMap::Builder builder;
    builder.add({"usa"}, {"united", "states"}, false);
    const SynonymMap map = builder.build();

    EXPECT_EQ(synonyms(map, "USA", true), (std::vector<std::string>{"united", "states"}));
    // Without the flag the rule does not fire.
    EXPECT_EQ(synonyms(map, "USA", false), (std::vector<std::string>{"USA"}));
}

TEST(SynonymFilterTest, LeavesUnknownWordsAlone) {
    SynonymMap::Builder builder;
    builder.add({"fast"}, {"quick"}, false);
    const SynonymMap map = builder.build();

    EXPECT_EQ(synonyms(map, "slow car"), (std::vector<std::string>{"slow", "car"}));
    EXPECT_EQ(synonym_positions(map, "slow car"), (std::vector<uint32_t>{1, 1}));
}
