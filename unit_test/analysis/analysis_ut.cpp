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

module lucene_core:ut.analysis;

import lucene_core;

using namespace lucene;

namespace {

struct Token {
    std::string term;
    uint32_t start;
    uint32_t end;
    uint32_t position_increment;

    bool operator==(const Token &other) const {
        return term == other.term && start == other.start && end == other.end &&
               position_increment == other.position_increment;
    }
};

std::vector<Token> collect(TokenStream &stream) {
    std::vector<Token> tokens;
    auto *term = stream.attributes().get_attribute<CharTermAttribute>();
    auto *offset = stream.attributes().get_attribute<OffsetAttribute>();
    auto *position_increment = stream.attributes().get_attribute<PositionIncrementAttribute>();
    while (true) {
        auto more = stream.increment_token();
        EXPECT_TRUE(more.has_value()) << (more ? "" : more.error().to_string());
        if (!more.value()) {
            break;
        }
        tokens.push_back(Token{std::string(term->text()),
                               offset->start_offset(),
                               offset->end_offset(),
                               position_increment->position_increment()});
    }
    return tokens;
}

std::vector<Token> analyze(Analyzer &analyzer, std::string_view text) {
    auto stream = analyzer.token_stream("field", text);
    EXPECT_TRUE(stream.has_value()) << (stream ? "" : stream.error().to_string());
    return collect(*stream.value());
}

std::vector<std::string> terms_of(const std::vector<Token> &tokens) {
    std::vector<std::string> terms;
    for (const auto &token : tokens) {
        terms.push_back(token.term);
    }
    return terms;
}

/// Same as [`Token`] plus the lexical type written by `StandardTokenizer`.
struct TypedToken {
    std::string term;
    std::string type;
    uint32_t start;
    uint32_t end;

    bool operator==(const TypedToken &other) const {
        return term == other.term && type == other.type && start == other.start && end == other.end;
    }
};

std::vector<TypedToken> collect_typed(TokenStream &stream) {
    std::vector<TypedToken> tokens;
    auto *term = stream.attributes().get_attribute<CharTermAttribute>();
    auto *type = stream.attributes().get_attribute<TypeAttribute>();
    auto *offset = stream.attributes().get_attribute<OffsetAttribute>();
    while (true) {
        auto more = stream.increment_token();
        EXPECT_TRUE(more.has_value()) << (more ? "" : more.error().to_string());
        if (!more.value()) {
            break;
        }
        tokens.push_back(
            TypedToken{std::string(term->text()), std::string(type->type()), offset->start_offset(), offset->end_offset()});
    }
    return tokens;
}

std::vector<TypedToken> tokenize_standard(std::string_view text, size_t max_token_length = 255) {
    StandardTokenizer tokenizer(max_token_length);
    EXPECT_TRUE(tokenizer.set_reader(text).has_value());
    EXPECT_TRUE(tokenizer.reset().has_value());
    return collect_typed(tokenizer);
}

std::vector<std::string> types_of(const std::vector<TypedToken> &tokens) {
    std::vector<std::string> types;
    for (const auto &token : tokens) {
        types.push_back(token.type);
    }
    return types;
}

std::vector<std::string> terms_of(const std::vector<TypedToken> &tokens) {
    std::vector<std::string> terms;
    for (const auto &token : tokens) {
        terms.push_back(token.term);
    }
    return terms;
}

} // namespace

TEST(AttributeSourceTest, AddAndLookupByType) {
    AttributeSource source;
    EXPECT_FALSE(source.has_attribute<CharTermAttribute>());
    auto *term = source.add_attribute<CharTermAttribute>();
    ASSERT_NE(term, nullptr);
    term->set_text("hello");
    EXPECT_TRUE(source.has_attribute<CharTermAttribute>());
    EXPECT_EQ(source.get_attribute<CharTermAttribute>()->text(), "hello");
    // Adding the same type returns the same instance.
    EXPECT_EQ(source.add_attribute<CharTermAttribute>(), term);

    source.end_attributes();
    EXPECT_TRUE(source.get_attribute<CharTermAttribute>()->text().empty());
    EXPECT_EQ(source.get_attribute<PositionIncrementAttribute>()
                  ? source.get_attribute<PositionIncrementAttribute>()->position_increment()
                  : 0u,
              0u);
}

TEST(AttributeSourceTest, CloneIsADeepCopy) {
    AttributeSource source;
    source.add_attribute<CharTermAttribute>()->set_text("abc");
    source.add_attribute<OffsetAttribute>()->set_offset(1, 4);

    AttributeSource copy(source);
    ASSERT_EQ(copy.size(), 2u);
    copy.get_attribute<CharTermAttribute>()->set_text("xyz");
    EXPECT_EQ(source.get_attribute<CharTermAttribute>()->text(), "abc");
    EXPECT_EQ(copy.get_attribute<OffsetAttribute>()->start_offset(), 1u);
}

TEST(CharTermAttributeTest, Utf8AppendAndSlice) {
    CharTermAttribute term;
    term.set_text("héllo");
    EXPECT_EQ(term.length(), 6u);
    EXPECT_EQ(term.char_at(1), U'é');
    EXPECT_EQ(term.to_u32string().size(), 5u);

    term.append(U'!');
    EXPECT_EQ(term.text(), "héllo!");
    term.append(" 日本");
    EXPECT_EQ(term.text(), "héllo! 日本");
    // `sub_sequence` takes byte offsets (the attribute stores UTF-8).
    EXPECT_EQ(term.sub_sequence(0, 3), "h\xc3\xa9");
    EXPECT_EQ(term.sub_sequence(0, 1), "h");
}

TEST(WhitespaceTokenizerTest, SplitsOnWhitespaceWithByteOffsets) {
    WhitespaceTokenizer tokenizer;
    ASSERT_TRUE(tokenizer.set_reader("  hello\tworld  ").has_value());
    ASSERT_TRUE(tokenizer.reset().has_value());
    const auto tokens = collect(tokenizer);
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].term, "hello");
    EXPECT_EQ(tokens[0].start, 2u);
    EXPECT_EQ(tokens[0].end, 7u);
    EXPECT_EQ(tokens[1].term, "world");
    EXPECT_EQ(tokens[1].start, 8u);
    EXPECT_EQ(tokens[1].end, 13u);
    // Note: Lucene's whitespace tokenizers fix up offsets; here they are the
    // raw byte positions of the token inside the input.
    EXPECT_EQ(tokens[1].position_increment, 1u);
}

TEST(WhitespaceTokenizerTest, Utf8Offsets) {
    WhitespaceTokenizer tokenizer;
    ASSERT_TRUE(tokenizer.set_reader("é x").has_value());
    const auto tokens = collect(tokenizer);
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].term, "é");
    EXPECT_EQ(tokens[0].start, 0u);
    EXPECT_EQ(tokens[0].end, 2u); // 2 UTF-8 bytes
    EXPECT_EQ(tokens[1].start, 3u);
}

TEST(LetterTokenizerTest, DropsDigitsAndPunctuation) {
    LetterTokenizer tokenizer;
    ASSERT_TRUE(tokenizer.set_reader("abc123-def").has_value());
    const auto tokens = collect(tokenizer);
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].term, "abc");
    EXPECT_EQ(tokens[1].term, "def");
}

TEST(KeywordTokenizerTest, WholeInputIsOneToken) {
    KeywordTokenizer tokenizer;
    ASSERT_TRUE(tokenizer.set_reader("Hello World").has_value());
    const auto tokens = collect(tokenizer);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].term, "Hello World");
    EXPECT_EQ(tokens[0].end, 11u);
}

TEST(LowerCaseFilterTest, LowerCasesAsciiAndLatin1) {
    LowerCaseFilter filter(std::make_unique<LetterTokenizer>());
    auto *tokenizer = dynamic_cast<Tokenizer *>(&filter.input());
    ASSERT_NE(tokenizer, nullptr);
    ASSERT_TRUE(tokenizer->set_reader("HÉLLO World").has_value());
    ASSERT_TRUE(filter.reset().has_value());
    const auto tokens = collect(filter);
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].term, "héllo");
    EXPECT_EQ(tokens[1].term, "world");
}

TEST(StopFilterTest, RemovesStopWordsAndKeepsPositions) {
    StopFilter filter(std::make_unique<WhitespaceTokenizer>(), {"the", "of"});
    auto *tokenizer = dynamic_cast<Tokenizer *>(&filter.input());
    ASSERT_NE(tokenizer, nullptr);
    ASSERT_TRUE(tokenizer->set_reader("the quick of fox").has_value());
    ASSERT_TRUE(filter.reset().has_value());
    const auto tokens = collect(filter);
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].term, "quick");
    // "the" consumed position 1, so "quick" is at position 2.
    EXPECT_EQ(tokens[0].position_increment, 2u);
    EXPECT_EQ(tokens[1].term, "fox");
    EXPECT_EQ(tokens[1].position_increment, 2u);
}

TEST(StopFilterTest, TakesACharArraySetInsteadOfAStdSet) {
    // The set the analyzers share is a `CharArraySet`, exactly like in the reference
    // implementation; a case-insensitive one also drops "The".
    CharArraySet stop_words(true);
    stop_words.add_str("the");
    stop_words.add_str("of");
    EXPECT_EQ(stop_words.size(), 2U);

    StopFilter filter(std::make_unique<WhitespaceTokenizer>(), stop_words);
    ASSERT_NE(filter.char_array_stop_words(), nullptr);
    EXPECT_TRUE(filter.char_array_stop_words()->ignore_case());
    auto *tokenizer = dynamic_cast<Tokenizer *>(&filter.input());
    ASSERT_NE(tokenizer, nullptr);
    ASSERT_TRUE(tokenizer->set_reader("The quick of fox").has_value());
    ASSERT_TRUE(filter.reset().has_value());
    const auto tokens = collect(filter);
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].term, "quick");
    EXPECT_EQ(tokens[1].term, "fox");
}

TEST(SimpleAnalyzerTest, ProducesLowerCasedLetterRuns) {
    SimpleAnalyzer analyzer;
    EXPECT_EQ(analyzer.reuse_strategy(), ReuseStrategy::Global);
    const auto tokens = analyze(analyzer, "The Quick, brown FOX!");
    EXPECT_EQ(terms_of(tokens), (std::vector<std::string>{"the", "quick", "brown", "fox"}));
    EXPECT_EQ(tokens[0].start, 0u);
    EXPECT_EQ(tokens[0].end, 3u);
}

TEST(WhitespaceAnalyzerTest, ReusesComponentsAcrossCalls) {
    WhitespaceAnalyzer analyzer;
    EXPECT_EQ(terms_of(analyze(analyzer, "one two")), (std::vector<std::string>{"one", "two"}));
    // Second call must restart the stream, not continue it.
    EXPECT_EQ(terms_of(analyze(analyzer, "three")), (std::vector<std::string>{"three"}));
    EXPECT_EQ(analyzer.to_string(), "WhitespaceAnalyzer");
}

TEST(KeywordAnalyzerTest, SingleTokenPerCall) {
    KeywordAnalyzer analyzer;
    const auto tokens = analyze(analyzer, "a b c");
    EXPECT_EQ(terms_of(tokens), (std::vector<std::string>{"a b c"}));
}

TEST(UnicodeUtilTest, WhitespaceAndClassification) {
    EXPECT_TRUE(char_is_whitespace(U' '));
    EXPECT_TRUE(char_is_whitespace(U'\t'));
    EXPECT_TRUE(char_is_whitespace(U'\u3000'));
    EXPECT_FALSE(char_is_whitespace(0x00A0));
    EXPECT_TRUE(char_is_letter(U'a'));
    EXPECT_TRUE(char_is_letter(U'日'));
    EXPECT_FALSE(char_is_letter(U'1'));
    EXPECT_TRUE(char_is_digit(U'7'));
    EXPECT_FALSE(char_is_digit(U'x'));
}

TEST(WordBreakClassTest, ClassifiesScriptsAndMarks) {
    EXPECT_EQ(word_break_class(U'a'), WordBreakClass::ALetter);
    EXPECT_EQ(word_break_class(U'7'), WordBreakClass::Numeric);
    EXPECT_EQ(word_break_class(U'_'), WordBreakClass::ExtendNumLet);
    EXPECT_EQ(word_break_class(U'.'), WordBreakClass::MidNumLet);
    EXPECT_EQ(word_break_class(U','), WordBreakClass::MidNum);
    EXPECT_EQ(word_break_class(U':'), WordBreakClass::MidLetter);
    EXPECT_EQ(word_break_class(U'\u0301'), WordBreakClass::Extend); // combining acute
    EXPECT_EQ(word_break_class(0x200D), WordBreakClass::Zwj);
    EXPECT_EQ(word_break_class(U'日'), WordBreakClass::Ideographic);
    EXPECT_EQ(word_break_class(U'ひ'), WordBreakClass::Hiragana);
    EXPECT_EQ(word_break_class(U'カ'), WordBreakClass::Katakana);
    EXPECT_EQ(word_break_class(U'한'), WordBreakClass::Hangul);
    EXPECT_EQ(word_break_class(0x1F1E6), WordBreakClass::RegionalIndicator);
    EXPECT_EQ(word_break_class(0x1F44D), WordBreakClass::Emoji);
    EXPECT_EQ(word_break_class(U' '), WordBreakClass::Other);
    EXPECT_EQ(word_break_class(U'!'), WordBreakClass::Other);
}

TEST(StandardTokenizerTest, SplitsAlphanumericAndNumericRuns) {
    const auto tokens = tokenize_standard("Hello, World 3.14 x2 1,234");
    const std::vector<TypedToken> expected{
        {"Hello", "<ALPHANUM>", 0, 5},
        {"World", "<ALPHANUM>", 7, 12},
        {"3.14", "<NUM>", 13, 17},
        {"x2", "<ALPHANUM>", 18, 20},
        {"1,234", "<NUM>", 21, 26},
    };
    EXPECT_EQ(tokens, expected);
}

TEST(StandardTokenizerTest, KeepsApostrophesAndDotsInsideWords) {
    // Straight and typographic apostrophes are both `MidNumLet`.
    EXPECT_EQ(terms_of(tokenize_standard("don't don\u2019t")),
              (std::vector<std::string>{"don't", "don\u2019t"}));
    EXPECT_EQ(terms_of(tokenize_standard("U.S.A")), (std::vector<std::string>{"U.S.A"}));
    // A trailing dot is not followed by a letter, so it ends the token.
    EXPECT_EQ(terms_of(tokenize_standard("end.")), (std::vector<std::string>{"end"}));
}

TEST(StandardTokenizerTest, IdeographsAreOneTokenEach) {
    const auto tokens = tokenize_standard("日本語");
    const std::vector<TypedToken> expected{
        {"日", "<IDEOGRAPHIC>", 0, 3},
        {"本", "<IDEOGRAPHIC>", 3, 6},
        {"語", "<IDEOGRAPHIC>", 6, 9},
    };
    EXPECT_EQ(tokens, expected);
}

TEST(StandardTokenizerTest, ScriptRunsStayTogether) {
    EXPECT_EQ(types_of(tokenize_standard("ひらがな")), (std::vector<std::string>{"<HIRAGANA>"}));
    EXPECT_EQ(types_of(tokenize_standard("カタカナ")), (std::vector<std::string>{"<KATAKANA>"}));
    EXPECT_EQ(types_of(tokenize_standard("한글")), (std::vector<std::string>{"<HANGUL>"}));
}

TEST(StandardTokenizerTest, EmojiSequencesAreOneToken) {
    // Thumbs up + skin tone modifier.
    auto tokens = tokenize_standard("\U0001F44D\U0001F3FD");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, "<EMOJI>");
    // Family: man ZWJ woman ZWJ girl.
    tokens = tokenize_standard("\U0001F468\u200D\U0001F469\u200D\U0001F467");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, "<EMOJI>");
}

TEST(StandardTokenizerTest, UnderscoreJoinsBothSides) {
    EXPECT_EQ(terms_of(tokenize_standard("snake_case 1_000")),
              (std::vector<std::string>{"snake_case", "1_000"}));
    EXPECT_EQ(types_of(tokenize_standard("snake_case 1_000")),
              (std::vector<std::string>{"<ALPHANUM>", "<NUM>"}));
}

TEST(StandardTokenizerTest, OverLongTokensAreSkippedAndPositionsAccumulate) {
    const auto tokens = tokenize_standard("abcdefgh ij kl", 4);
    // `abcdefgh` is dropped; the next token carries the skipped position.
    ASSERT_EQ(terms_of(tokens), (std::vector<std::string>{"ij", "kl"}));
    StandardTokenizer tokenizer(4);
    ASSERT_TRUE(tokenizer.set_reader("abcdefgh ij kl").has_value());
    ASSERT_TRUE(tokenizer.reset().has_value());
    auto *position_increment = tokenizer.attributes().get_attribute<PositionIncrementAttribute>();
    ASSERT_NE(position_increment, nullptr);
    ASSERT_TRUE(tokenizer.increment_token().value());
    EXPECT_EQ(position_increment->position_increment(), 2u);
    ASSERT_TRUE(tokenizer.increment_token().value());
    EXPECT_EQ(position_increment->position_increment(), 1u);
    EXPECT_FALSE(tokenizer.increment_token().value());
}

TEST(StandardTokenizerTest, MaxTokenLengthIsValidated) {
    StandardTokenizer tokenizer;
    EXPECT_EQ(tokenizer.max_token_length(), StandardTokenizer::DEFAULT_MAX_TOKEN_LENGTH);
    EXPECT_TRUE(tokenizer.set_max_token_length(64).has_value());
    EXPECT_EQ(tokenizer.max_token_length(), 64u);
    auto too_small = tokenizer.set_max_token_length(0);
    ASSERT_FALSE(too_small.has_value());
    EXPECT_EQ(too_small.error().kind(), ErrorKind::kIllegalArgument);
    auto too_large = tokenizer.set_max_token_length(StandardTokenizer::MAX_TOKEN_LENGTH_LIMIT + 1);
    ASSERT_FALSE(too_large.has_value());
    EXPECT_EQ(too_large.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(StandardAnalyzerTest, LowerCasesAndDropsStopWords) {
    StandardAnalyzer analyzer;
    EXPECT_EQ(analyzer.to_string(), "StandardAnalyzer");
    auto stream = analyzer.token_stream("field", "The Quick, brown FOX!");
    ASSERT_TRUE(stream.has_value()) << (stream ? "" : stream.error().to_string());
    auto *term = stream.value()->attributes().get_attribute<CharTermAttribute>();
    auto *position_increment = stream.value()->attributes().get_attribute<PositionIncrementAttribute>();
    ASSERT_NE(term, nullptr);
    ASSERT_NE(position_increment, nullptr);

    std::vector<std::string> terms;
    std::vector<uint32_t> positions;
    while (stream.value()->increment_token().value()) {
        terms.push_back(std::string(term->text()));
        positions.push_back(position_increment->position_increment());
    }
    EXPECT_EQ(terms, (std::vector<std::string>{"quick", "brown", "fox"}));
    // "the" occupied position 1.
    EXPECT_EQ(positions, (std::vector<uint32_t>{2, 1, 1}));
}

TEST(StandardAnalyzerTest, CustomStopWordsAndMaxTokenLength) {
    StandardAnalyzer analyzer({"quick"});
    EXPECT_EQ(analyzer.stop_words().count("quick"), 1u);
    auto stream = analyzer.token_stream("field", "quick slower");
    ASSERT_TRUE(stream.has_value());
    auto *term = stream.value()->attributes().get_attribute<CharTermAttribute>();
    ASSERT_TRUE(stream.value()->increment_token().value());
    EXPECT_EQ(term->text(), "slower");
    EXPECT_FALSE(stream.value()->increment_token().value());

    EXPECT_TRUE(analyzer.set_max_token_length(8).has_value());
    EXPECT_EQ(analyzer.max_token_length(), 8u);
    EXPECT_FALSE(analyzer.set_max_token_length(0).has_value());
}
