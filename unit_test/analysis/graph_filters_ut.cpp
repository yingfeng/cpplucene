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
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

module lucene_core:ut.graph_filters;

import lucene_core;

using namespace lucene;

namespace {

/// One emitted token, with the position it was placed at.
struct Out {
    std::string term;
    int32_t pos{0};
    uint32_t position_length{1};
    uint32_t start_offset{0};
    uint32_t end_offset{0};
};

std::vector<Out> collect(Tokenizer &source, TokenStream &head, std::string_view text) {
    std::vector<Out> out;
    EXPECT_TRUE(source.set_reader(text).has_value());
    EXPECT_TRUE(head.reset().has_value());
    int32_t pos = -1;
    while (true) {
        auto more = head.increment_token();
        EXPECT_TRUE(more.has_value()) << (more.has_value() ? "" : more.error().to_string());
        if (!more.has_value() || !more.value()) {
            break;
        }
        auto *term = head.attributes().get_attribute<CharTermAttribute>();
        auto *pos_inc = head.attributes().get_attribute<PositionIncrementAttribute>();
        auto *pos_len = head.attributes().get_attribute<PositionLengthAttribute>();
        auto *offset = head.attributes().get_attribute<OffsetAttribute>();
        pos += static_cast<int32_t>(pos_inc->position_increment());
        out.push_back(Out{std::string(term->text()), pos, pos_len->position_length(), offset->start_offset(),
                          offset->end_offset()});
    }
    EXPECT_TRUE(head.end().has_value());
    return out;
}

/// Runs a single [`WordDelimiterGraphFilter`] over the whitespace tokens of `text`.
std::vector<Out> run_word_delimiter(std::string_view text, uint32_t flags, const CharArraySet *prot_words = nullptr,
                                    bool adjust_internal_offsets = false) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    auto head = std::make_unique<WordDelimiterGraphFilter>(std::move(tokenizer), flags, prot_words,
                                                           adjust_internal_offsets);
    return collect(*source, *head, text);
}

std::vector<std::string> terms_of(const std::vector<Out> &tokens) {
    std::vector<std::string> terms;
    terms.reserve(tokens.size());
    for (const Out &token : tokens) {
        terms.push_back(token.term);
    }
    return terms;
}

/// A `WhitespaceTokenizer -> LowerCaseFilter -> WordDelimiterGraphFilter [-> FlattenGraphFilter]` chain.
class WordDelimiterGraphAnalyzer final : public Analyzer {
public:
    WordDelimiterGraphAnalyzer(uint32_t flags, bool flatten) : flags_(flags), flatten_(flatten) {}

    [[nodiscard]] std::string to_string() const override { return "WordDelimiterGraphAnalyzer"; }

protected:
    [[nodiscard]] Result<std::unique_ptr<TokenStreamComponents>> create_components(std::string_view) override {
        auto tokenizer = std::make_unique<WhitespaceTokenizer>();
        auto *source = tokenizer.get();
        std::unique_ptr<TokenStream> chain = std::make_unique<WordDelimiterGraphFilter>(std::move(tokenizer), flags_);
        chain = std::make_unique<LowerCaseFilter>(std::move(chain));
        if (flatten_) {
            chain = std::make_unique<FlattenGraphFilter>(std::move(chain));
        }
        return std::make_unique<TokenStreamComponents>(source, std::move(chain));
    }

private:
    uint32_t flags_;
    bool flatten_;
};

constexpr uint32_t kPartsFlags = WordDelimiterGraphFilter::kGenerateWordParts |
                                 WordDelimiterGraphFilter::kGenerateNumberParts |
                                 WordDelimiterGraphFilter::kSplitOnCaseChange |
                                 WordDelimiterGraphFilter::kSplitOnNumerics;

} // namespace

TEST(WordDelimiterGraphFilterTest, SplitsOnCaseAndDelimiters) {
    const auto split = run_word_delimiter("PowerShot", kPartsFlags);
    EXPECT_EQ(terms_of(split), (std::vector<std::string>{"Power", "Shot"}));
    ASSERT_EQ(split.size(), 2U);
    EXPECT_EQ(split[0].pos, 0);
    EXPECT_EQ(split[1].pos, 1);
    // `adjust_internal_offsets` is off, so every part repeats the whole token's offsets.
    EXPECT_EQ(split[0].start_offset, 0U);
    EXPECT_EQ(split[0].end_offset, 9U);
    EXPECT_EQ(split[1].end_offset, 9U);

    const auto hyphens = run_word_delimiter("basic-split", kPartsFlags);
    EXPECT_EQ(terms_of(hyphens), (std::vector<std::string>{"basic", "split"}));
    EXPECT_EQ(hyphens[0].pos, 0);
    EXPECT_EQ(hyphens[1].pos, 1);
    EXPECT_EQ(hyphens[0].position_length, 1U);
}

TEST(WordDelimiterGraphFilterTest, KeepsASingleWordWhole) {
    const auto single = run_word_delimiter("houses", kPartsFlags);
    ASSERT_EQ(single.size(), 1U);
    EXPECT_EQ(single[0].term, "houses");
    EXPECT_EQ(single[0].pos, 0);
    EXPECT_EQ(single[0].position_length, 1U);
}

TEST(WordDelimiterGraphFilterTest, CatenatesWordsIntoAGraphEdge) {
    const auto tokens = run_word_delimiter("wi-fi", kPartsFlags | WordDelimiterGraphFilter::kCatenateWords);
    // The concatenation is emitted first at the start node and spans both subwords.
    EXPECT_EQ(terms_of(tokens), (std::vector<std::string>{"wifi", "wi", "fi"}));
    ASSERT_EQ(tokens.size(), 3U);
    EXPECT_EQ(tokens[0].pos, 0);
    EXPECT_EQ(tokens[0].position_length, 2U);
    EXPECT_EQ(tokens[1].pos, 0);
    EXPECT_EQ(tokens[1].position_length, 1U);
    EXPECT_EQ(tokens[2].pos, 1);
    EXPECT_EQ(tokens[2].position_length, 1U);
}

TEST(WordDelimiterGraphFilterTest, SplitsAndCatenatesNumbers) {
    const auto tokens = run_word_delimiter("SD500-42", kPartsFlags | WordDelimiterGraphFilter::kCatenateNumbers);
    EXPECT_EQ(terms_of(tokens), (std::vector<std::string>{"SD", "50042", "500", "42"}));
    ASSERT_EQ(tokens.size(), 4U);
    EXPECT_EQ(tokens[0].pos, 0);
    EXPECT_EQ(tokens[1].pos, 1);
    EXPECT_EQ(tokens[1].position_length, 2U);
    EXPECT_EQ(tokens[2].pos, 1);
    EXPECT_EQ(tokens[3].pos, 2);
}

TEST(WordDelimiterGraphFilterTest, PreservesTheOriginalToken) {
    const auto tokens = run_word_delimiter("500-42", WordDelimiterGraphFilter::kGenerateNumberParts |
                                                          WordDelimiterGraphFilter::kPreserveOriginal);
    EXPECT_EQ(terms_of(tokens), (std::vector<std::string>{"500-42", "500", "42"}));
    ASSERT_EQ(tokens.size(), 3U);
    // The original comes first, keeps the incoming position increment and spans the parts.
    EXPECT_EQ(tokens[0].pos, 0);
    EXPECT_EQ(tokens[0].position_length, 2U);
    EXPECT_EQ(tokens[1].pos, 0);
    EXPECT_EQ(tokens[2].pos, 1);
}

TEST(WordDelimiterGraphFilterTest, StemsEnglishPossessives) {
    const auto stemmed = run_word_delimiter("ra's", kPartsFlags | WordDelimiterGraphFilter::kStemEnglishPossessive);
    EXPECT_EQ(terms_of(stemmed), (std::vector<std::string>{"ra"}));

    const auto kept = run_word_delimiter("ra's", kPartsFlags);
    EXPECT_EQ(terms_of(kept), (std::vector<std::string>{"ra", "s"}));

    // A possessive followed by a delimiter is still recognised.
    const auto trailing = run_word_delimiter("test's'", kPartsFlags | WordDelimiterGraphFilter::kStemEnglishPossessive);
    EXPECT_EQ(terms_of(trailing), (std::vector<std::string>{"test"}));
}

TEST(WordDelimiterGraphFilterTest, AdjustsInternalOffsets) {
    const auto tokens = run_word_delimiter("foo-bar", kPartsFlags, nullptr, true);
    ASSERT_EQ(tokens.size(), 2U);
    EXPECT_EQ(tokens[0].term, "foo");
    EXPECT_EQ(tokens[0].start_offset, 0U);
    EXPECT_EQ(tokens[0].end_offset, 3U);
    EXPECT_EQ(tokens[1].term, "bar");
    EXPECT_EQ(tokens[1].start_offset, 4U);
    EXPECT_EQ(tokens[1].end_offset, 7U);
}

TEST(WordDelimiterGraphFilterTest, ProtectedWordsAndKeywordsAreLeftAlone) {
    CharArraySet prot_words(true);
    prot_words.add_str("wi-fi");
    const auto protected_tokens =
        run_word_delimiter("wi-fi", kPartsFlags, &prot_words);
    ASSERT_EQ(protected_tokens.size(), 1U);
    EXPECT_EQ(protected_tokens[0].term, "wi-fi");

    // `kIgnoreKeywords` passes a keyword token through untouched.
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    auto head = std::make_unique<WordDelimiterGraphFilter>(
        std::move(tokenizer), kPartsFlags | WordDelimiterGraphFilter::kIgnoreKeywords);
    EXPECT_TRUE(source->set_reader("PowerShot").has_value());
    EXPECT_TRUE(head->reset().has_value());
    head->attributes().get_attribute<KeywordAttribute>()->set_keyword(true);
    auto more = head->increment_token();
    ASSERT_TRUE(more.has_value());
    ASSERT_TRUE(more.value());
    EXPECT_EQ(head->attributes().get_attribute<CharTermAttribute>()->text(), "PowerShot");
    auto done = head->increment_token();
    ASSERT_TRUE(done.has_value());
    EXPECT_FALSE(done.value());
}

TEST(WordDelimiterGraphFilterTest, DelimiterOnlyTokensLeaveAHole) {
    const auto tokens = run_word_delimiter("a --- b", kPartsFlags);
    ASSERT_EQ(tokens.size(), 2U);
    EXPECT_EQ(tokens[0].term, "a");
    EXPECT_EQ(tokens[0].pos, 0);
    // The swallowed token still advances the position of the next one.
    EXPECT_EQ(tokens[1].term, "b");
    EXPECT_EQ(tokens[1].pos, 2);

    const auto only_delimiters = run_word_delimiter("---", kPartsFlags);
    EXPECT_TRUE(only_delimiters.empty());
}

TEST(WordDelimiterIteratorTest, ClassifiesCodePoints) {
    using WDI = WordDelimiterIterator;
    EXPECT_EQ(WDI::get_type(U'a'), WDI::kLower);
    EXPECT_EQ(WDI::get_type(U'A'), WDI::kUpper);
    EXPECT_EQ(WDI::get_type(U'7'), WDI::kDigit);
    EXPECT_EQ(WDI::get_type(U'-'), WDI::kSubwordDelim);
    // A CJK letter has no case, so it is ALPHA rather than LOWER.
    EXPECT_EQ(WDI::get_type(U'\u4E2D'), WDI::kAlpha);
    // Unpaired bytes never glue words together.
    EXPECT_EQ(WDI::get_type(static_cast<char32_t>(0xFFFFFFFFu)), WDI::kSubwordDelim);

    WDI iterator("camelCase", true, true, true);
    ASSERT_EQ(iterator.next(), 5);
    EXPECT_EQ(iterator.current(), 0);
    EXPECT_FALSE(iterator.is_single_word());
    ASSERT_EQ(iterator.next(), 9);
    EXPECT_EQ(iterator.current(), 5);
    EXPECT_EQ(iterator.next(), WDI::kDone);
}

TEST(SynonymGraphFilterTest, MultiWordAlternativeFormsASidePath) {
    SynonymMap::Builder builder;
    builder.add({"a", "b"}, {"x", "y"}, true);
    const SynonymMap map = builder.build();

    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    auto head = std::make_unique<SynonymGraphFilter>(std::move(tokenizer), map, false);
    const auto tokens = collect(*source, *head, "a b c");

    // Position 0 fans out into `x` (synonym) and `a` (original), `y` continues the
    // synonym path up to node 2, and `b` closes the original path there.
    EXPECT_EQ(terms_of(tokens), (std::vector<std::string>{"x", "a", "y", "b", "c"}));
    ASSERT_EQ(tokens.size(), 5U);
    EXPECT_EQ(tokens[0].pos, 0);
    EXPECT_EQ(tokens[0].position_length, 1U);
    EXPECT_EQ(tokens[1].pos, 0);
    EXPECT_EQ(tokens[1].position_length, 2U);
    EXPECT_EQ(tokens[2].pos, 1);
    EXPECT_EQ(tokens[2].position_length, 2U);
    EXPECT_EQ(tokens[3].pos, 2);
    EXPECT_EQ(tokens[3].position_length, 1U);
    EXPECT_EQ(tokens[4].pos, 3);
}

TEST(SynonymGraphFilterTest, SingleWordRulesAndUnmatchedInput) {
    SynonymMap::Builder builder;
    builder.add({"nyc"}, {"new", "york"}, false);
    const SynonymMap map = builder.build();

    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    auto head = std::make_unique<SynonymGraphFilter>(std::move(tokenizer), map, true);
    const auto tokens = collect(*source, *head, "NYC here");

    EXPECT_EQ(terms_of(tokens), (std::vector<std::string>{"new", "york", "here"}));
    ASSERT_EQ(tokens.size(), 3U);
    EXPECT_EQ(tokens[0].pos, 0);
    EXPECT_EQ(tokens[0].position_length, 1U);
    EXPECT_EQ(tokens[1].pos, 1);
    // The rule drops the matched word, so `here` still comes one position later.
    EXPECT_EQ(tokens[2].pos, 2);
}

TEST(FlattenGraphFilterTest, LeavesAFlatStreamAlone) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    auto head = std::make_unique<FlattenGraphFilter>(std::move(tokenizer));
    const auto tokens = collect(*source, *head, "one two three");

    EXPECT_EQ(terms_of(tokens), (std::vector<std::string>{"one", "two", "three"}));
    for (size_t i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].pos, static_cast<int32_t>(i));
        EXPECT_EQ(tokens[i].position_length, 1U);
    }
    EXPECT_EQ(tokens[0].start_offset, 0U);
    EXPECT_EQ(tokens[0].end_offset, 3U);
    EXPECT_EQ(tokens[2].start_offset, 8U);
    EXPECT_EQ(tokens[2].end_offset, 13U);
}

TEST(FlattenGraphFilterTest, SausagesAGraphWithAHole) {
    // A three word alternative over a two word input leaves an output position that
    // only the synonym path reaches; flattening has to renumber it so the chain
    // stays contiguous.
    SynonymMap::Builder builder;
    builder.add({"a", "b"}, {"x", "y", "z"}, true);
    const SynonymMap map = builder.build();

    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    auto synonym = std::make_unique<SynonymGraphFilter>(std::move(tokenizer), map, false);
    auto head = std::make_unique<FlattenGraphFilter>(std::move(synonym));
    const auto tokens = collect(*source, *head, "a b c");

    EXPECT_EQ(terms_of(tokens), (std::vector<std::string>{"x", "a", "y", "b", "z", "c"}));
    ASSERT_EQ(tokens.size(), 6U);
    EXPECT_EQ(tokens[0].pos, 0);
    EXPECT_EQ(tokens[1].pos, 0);
    EXPECT_EQ(tokens[2].pos, 1);
    EXPECT_EQ(tokens[3].pos, 1);
    EXPECT_EQ(tokens[4].pos, 2);
    EXPECT_EQ(tokens[5].pos, 3);
    // Every token covers at least one position and the chain never jumps over a node.
    for (size_t i = 0; i < tokens.size(); ++i) {
        EXPECT_GE(tokens[i].position_length, 1U) << tokens[i].term;
        if (i > 0) {
            const int32_t step = tokens[i].pos - tokens[i - 1].pos;
            EXPECT_TRUE(step == 0 || step == 1) << tokens[i].term;
        }
    }
}

TEST(FlattenGraphFilterTest, MergesParallelPathsIntoOnePosition) {
    // Two paths over the same two input words: after flattening the nodes are
    // merged pairwise, so both alternatives share one position.
    SynonymMap::Builder builder;
    builder.add({"a", "b"}, {"x", "y"}, true);
    const SynonymMap map = builder.build();

    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *source = tokenizer.get();
    auto synonym = std::make_unique<SynonymGraphFilter>(std::move(tokenizer), map, false);
    auto head = std::make_unique<FlattenGraphFilter>(std::move(synonym));
    const auto tokens = collect(*source, *head, "a b");

    ASSERT_EQ(tokens.size(), 4U);
    EXPECT_EQ(terms_of(tokens), (std::vector<std::string>{"x", "a", "y", "b"}));
    EXPECT_EQ(tokens[0].pos, 0);
    EXPECT_EQ(tokens[1].pos, 0);
    EXPECT_EQ(tokens[2].pos, 1);
    EXPECT_EQ(tokens[3].pos, 1);
    for (const Out &token : tokens) {
        EXPECT_EQ(token.position_length, 1U);
    }
}

TEST(GraphFilterEndToEndTest, IndexesADelimitedWordsGraph) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_graph_filters";
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    FieldType body_type;
    ASSERT_TRUE(body_type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());

    {
        auto directory = FSDirectory::open(path);
        ASSERT_TRUE(directory.has_value()) << (directory.has_value() ? "" : directory.error().to_string());

        WordDelimiterGraphAnalyzer analyzer(kPartsFlags | WordDelimiterGraphFilter::kCatenateWords |
                                                WordDelimiterGraphFilter::kStemEnglishPossessive,
                                            true);
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(*directory.value(), config);
        ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

        for (const std::string &text : {"Wi-Fi works", "PowerShot camera"}) {
            Document doc;
            doc.add(std::make_unique<Field>("body", FieldValue{text}, body_type));
            ASSERT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
        }
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    auto reader = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    const auto count = [&searcher](const std::string &term) {
        auto hits = searcher.count(TermQuery(Term::from_text("body", term)));
        EXPECT_TRUE(hits.has_value());
        return hits.value_or(-1);
    };

    // The subwords and the concatenation are both indexed.
    EXPECT_EQ(count("wi"), 1);
    EXPECT_EQ(count("fi"), 1);
    EXPECT_EQ(count("wifi"), 1);
    EXPECT_EQ(count("powershot"), 1);
    EXPECT_EQ(count("power"), 1);
    EXPECT_EQ(count("shot"), 1);
    EXPECT_EQ(count("missing"), 0);

    // `wifi` and `fi` are adjacent in the flattened graph, so a phrase matches.
    auto phrase = PhraseQuery::create("body", {Term::from_text("body", "wifi"), Term::from_text("body", "fi")});
    ASSERT_TRUE(phrase.has_value()) << (phrase.has_value() ? "" : phrase.error().to_string());
    auto phrase_hits = searcher.count(phrase.value());
    EXPECT_TRUE(phrase_hits.has_value());
    EXPECT_EQ(phrase_hits.value_or(-1), 1);
}
