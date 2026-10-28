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

// A conjunction whose children need two-phase confirmation (phrase queries): the
// documents have to be exactly the ones every clause confirms.

module;

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.conjunction_two_phase;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kBody = "body";

FieldType body_type() {
    FieldType type;
    // A phrase needs positions.
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    return type;
}

/// Five documents whose bodies differ in which adjacent pairs they hold.
void build_index(Directory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    const std::vector<std::string> bodies{"a b c", "a b", "b c", "a c", "a b c a b c"};
    for (const std::string &body : bodies) {
        Document document;
        document.add(std::make_unique<Field>(std::string(kBody), FieldValue{body}, body_type()));
        ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());
}

std::shared_ptr<Query> phrase(std::string_view first, std::string_view second) {
    auto query = PhraseQuery::create(std::string(kBody),
                                     {Term::from_text(std::string(kBody), std::string(first)),
                                      Term::from_text(std::string(kBody), std::string(second))});
    EXPECT_TRUE(query.has_value()) << (query.has_value() ? "" : query.error().to_string());
    return std::make_shared<PhraseQuery>(std::move(query.value()));
}

std::shared_ptr<Query> conjunction_of(bool with_term) {
    BooleanQuery::Builder builder;
    EXPECT_TRUE(builder.add(phrase("a", "b"), Occur::Must).has_value());
    EXPECT_TRUE(builder.add(phrase("b", "c"), Occur::Must).has_value());
    if (with_term) {
        EXPECT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text(std::string(kBody), "a")), Occur::Must)
                            .has_value());
    }
    auto built = builder.build();
    EXPECT_TRUE(built.has_value()) << (built.has_value() ? "" : built.error().to_string());
    return std::make_shared<BooleanQuery>(std::move(built.value()));
}

std::vector<int32_t> docs_of(const TopDocs &top) {
    std::vector<int32_t> docs;
    for (const ScoreDoc &hit : top.score_docs) {
        docs.push_back(hit.doc);
    }
    std::sort(docs.begin(), docs.end());
    return docs;
}

} // namespace

TEST(ConjunctionTwoPhaseTest, BothPhrasesHaveToConfirmTheDocument) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    const IndexSearcher searcher(reader.value());

    // Documents 0 and 4 hold "a b" and "b c" adjacent.
    const std::vector<int32_t> expected{0, 4};
    for (const bool with_term : {false, true}) {
        const auto query = conjunction_of(with_term);

        auto counted = searcher.count(*query);
        ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
        EXPECT_EQ(static_cast<size_t>(counted.value()), expected.size()) << with_term;

        auto top = searcher.search(*query, 5);
        ASSERT_TRUE(top.has_value()) << top.error().to_string();
        EXPECT_EQ(docs_of(top.value()), expected) << with_term;
        EXPECT_EQ(top.value().total_hits.value, expected.size()) << with_term;
    }
}

TEST(ConjunctionTwoPhaseTest, ASinglePhraseStillNeedsItsConfirmation) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    const IndexSearcher searcher(reader.value());

    // "a b" alone matches documents 0, 1 and 4 (the approximation also visits 3).
    auto top = searcher.search(*phrase("a", "b"), 5);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(docs_of(top.value()), (std::vector<int32_t>{0, 1, 4}));
}
