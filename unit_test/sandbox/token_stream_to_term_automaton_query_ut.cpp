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
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.token_stream_to_term_automaton_query;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kId = "id";
constexpr std::string_view kText = "text";

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

FieldType keyword_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

const std::vector<std::pair<std::string, std::string>> &corpus() {
    static const std::vector<std::pair<std::string, std::string>> documents{
            {"1", "a b c"},
            {"2", "b a"},
            {"3", "a c"},
            {"4", "a b b c"},
            {"5", "x y"},
    };
    return documents;
}

void build_index(Directory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    for (const auto &[id, text] : corpus()) {
        Document doc;
        doc.add(std::make_unique<Field>(std::string(kId), FieldValue{id}, keyword_type()));
        doc.add(std::make_unique<Field>(std::string(kText), FieldValue{text}, text_type()));
        ASSERT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
}

std::vector<std::string> ids_of(IndexSearcher &searcher, const Query &query) {
    auto hits = searcher.search(query, 10);
    EXPECT_TRUE(hits.has_value()) << (hits ? "" : hits.error().to_string());
    std::vector<std::string> ids;
    if (!hits.has_value()) {
        return ids;
    }
    auto stored = searcher.reader().stored_fields();
    EXPECT_TRUE(stored.has_value());
    if (!stored.has_value()) {
        return ids;
    }
    for (const ScoreDoc &hit : hits.value().score_docs) {
        auto doc = stored.value()->document(hit.doc);
        EXPECT_TRUE(doc.has_value());
        if (!doc.has_value()) {
            continue;
        }
        auto values = doc.value().get_values(std::string(kId));
        if (values.has_value() && !values.value().empty()) {
            ids.push_back(values.value().front());
        }
    }
    std::ranges::sort(ids);
    return ids;
}

/// A token stream that emits a fixed script, so the test controls every
/// position increment.
class ScriptedStream final : public TokenStream {
public:
    struct Token {
        std::string text;
        uint32_t position_increment{1};
        uint32_t position_length{1};
    };

    explicit ScriptedStream(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    [[nodiscard]] AttributeSource &attributes() override { return source_; }

    [[nodiscard]] VoidResult reset() override {
        index_ = -1;
        return {};
    }

    [[nodiscard]] Result<bool> increment_token() override {
        ++index_;
        if (index_ >= tokens_.size()) {
            return false;
        }
        const Token &token = tokens_[index_];
        source_.add_attribute<CharTermAttribute>()->set_text(token.text);
        source_.add_attribute<PositionIncrementAttribute>()->set_position_increment(token.position_increment);
        source_.add_attribute<PositionLengthAttribute>()->set_position_length(token.position_length);
        return true;
    }

private:
    std::vector<Token> tokens_;
    size_t index_{static_cast<size_t>(-1)};
    AttributeSource source_;
};

} // namespace

TEST(TokenStreamToTermAutomatonQueryTest, BuildsAPhraseAutomaton) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    TokenStreamToTermAutomatonQuery builder;
    ScriptedStream stream({{"a"}, {"b"}});
    auto query = builder.to_query(std::string(kText), stream);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());

    // The same documents the phrase "a b" matches.
    EXPECT_EQ(ids_of(searcher, query.value()), (std::vector<std::string>{"1", "4"}));

    auto count = searcher.count(query.value());
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 2);
}

TEST(TokenStreamToTermAutomatonQueryTest, MapsAStarTokenToAnAnyTransition) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    TokenStreamToTermAutomatonQuery builder;
    ScriptedStream stream({{"a"}, {"*"}, {"c"}});
    auto query = builder.to_query(std::string(kText), stream);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    EXPECT_EQ(query.value().any_term_id(), 1);

    EXPECT_EQ(ids_of(searcher, query.value()), (std::vector<std::string>{"1"}));
}

TEST(TokenStreamToTermAutomatonQueryTest, HandlesPositionIncrements) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // A hole cannot be expressed by the automaton.
    TokenStreamToTermAutomatonQuery strict;
    ScriptedStream holed({{"a"}, {"b", 2}});
    auto hole = strict.to_query(std::string(kText), holed);
    EXPECT_FALSE(hole.has_value());

    // Unless the caller asks for the increments to be squeezed out: then "a" and
    // "b" become adjacent.
    TokenStreamToTermAutomatonQuery squeezed;
    squeezed.set_preserve_position_increments(false);
    ScriptedStream squeezed_stream({{"a"}, {"b", 2}});
    auto query = squeezed.to_query(std::string(kText), squeezed_stream);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    EXPECT_EQ(ids_of(searcher, query.value()), (std::vector<std::string>{"1", "4"}));
}

TEST(TokenStreamToTermAutomatonQueryTest, RejectsAnEmptyStream) {
    TokenStreamToTermAutomatonQuery builder;
    ScriptedStream empty({});
    auto query = builder.to_query(std::string(kText), empty);
    EXPECT_FALSE(query.has_value());
}
