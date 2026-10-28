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

// `MultiCollector` feeds several collectors from one search.  A collector that
// wants to skip low-scoring hits must not make the shared scorer hide documents
// another collector still needs, and a collector that stops early must not stop
// the ones that are still going.

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.multi_collector;

import lucene_core;

using namespace lucene;

namespace {

constexpr int32_t kDocs = 60;
constexpr std::string_view kBody = "body";
constexpr std::string_view kRare = "rare";

FieldType body_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    return type;
}

/// Builds `kDocs` documents that all hold "body", and every third one "rare".
/// The scores therefore differ, which is what a minimum competitive score needs.
void build_index(Directory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    for (int32_t doc = 0; doc < kDocs; ++doc) {
        Document document;
        document.add(std::make_unique<Field>(std::string(kBody), FieldValue{std::string("alpha")}, body_type()));
        if (doc % 3 == 0) {
            document.add(std::make_unique<Field>(std::string(kRare), FieldValue{std::string("rare")}, body_type()));
        }
        ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// A conjunction of both terms: a scorer that honours a minimum competitive
/// score, which is what makes the fan-out observable.
std::shared_ptr<Query> conjunction() {
    BooleanQuery::Builder builder;
    EXPECT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text(std::string(kBody), "alpha")), Occur::Must)
                        .has_value());
    EXPECT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text(std::string(kRare), "rare")), Occur::Must)
                        .has_value());
    auto built = builder.build();
    EXPECT_TRUE(built.has_value());
    return std::make_shared<BooleanQuery>(std::move(built.value()));
}

std::vector<int32_t> docs_of(const TopDocs &top) {
    std::vector<int32_t> docs;
    for (const ScoreDoc &hit : top.score_docs) {
        docs.push_back(hit.doc);
    }
    return docs;
}

} // namespace

TEST(MultiCollectorTest, ATopKCollectorDoesNotHideHitsFromTheCounter) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    const IndexSearcher searcher(reader.value());
    const auto query = conjunction();

    // A tiny threshold lets the top-k collector ask for a minimum score; the
    // counter needs every document, so that threshold must not reach the scorer.
    auto top = TopScoreDocCollector::create(5, std::nullopt, /*total_hits_threshold=*/5, nullptr);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    TotalHitCountCollector counter;
    auto multi = MultiCollector::wrap({top.value().get(), &counter});
    ASSERT_TRUE(multi.has_value()) << multi.error().to_string();

    ASSERT_TRUE(searcher.search_with_collector(*query, *multi.value()).has_value());
    // Every third document of sixty matches the conjunction.
    EXPECT_EQ(counter.total_hits(), kDocs / 3);

    auto hits = top.value()->top_docs();
    ASSERT_TRUE(hits.has_value()) << hits.error().to_string();
    // The top five are the same as a search with the top-k collector alone.
    auto alone = searcher.search(*query, 5);
    ASSERT_TRUE(alone.has_value()) << alone.error().to_string();
    EXPECT_EQ(docs_of(hits.value()), docs_of(alone.value()));
}

TEST(MultiCollectorTest, TwoTopKCollectorsAgreeWithSeparateSearches) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    const IndexSearcher searcher(reader.value());
    const auto query = conjunction();

    auto five = TopScoreDocCollector::create(5, std::nullopt, /*total_hits_threshold=*/3, nullptr);
    ASSERT_TRUE(five.has_value());
    auto ten = TopScoreDocCollector::create(10, std::nullopt, /*total_hits_threshold=*/3, nullptr);
    ASSERT_TRUE(ten.has_value());
    auto multi = MultiCollector::wrap({five.value().get(), ten.value().get()});
    ASSERT_TRUE(multi.has_value()) << multi.error().to_string();
    ASSERT_TRUE(searcher.search_with_collector(*query, *multi.value()).has_value());

    auto alone_five = searcher.search(*query, 5);
    ASSERT_TRUE(alone_five.has_value());
    auto alone_ten = searcher.search(*query, 10);
    ASSERT_TRUE(alone_ten.has_value());

    auto five_hits = five.value()->top_docs();
    ASSERT_TRUE(five_hits.has_value());
    auto ten_hits = ten.value()->top_docs();
    ASSERT_TRUE(ten_hits.has_value());
    EXPECT_EQ(docs_of(five_hits.value()), docs_of(alone_five.value()));
    EXPECT_EQ(docs_of(ten_hits.value()), docs_of(alone_ten.value()));
}

TEST(MultiCollectorTest, ACollectorThatCountsEveryHitKeepsTheLeafGoing) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    const IndexSearcher searcher(reader.value());
    const auto query = conjunction();

    // With `max_buffered_docs` defaults this index has several segments, so the
    // early termination of one collector happens per leaf while the counter is
    // still interested: the count has to stay exact.
    auto top = TopScoreDocCollector::create(2, std::nullopt, /*total_hits_threshold=*/2, nullptr);
    ASSERT_TRUE(top.has_value());
    TotalHitCountCollector counter;
    auto multi = MultiCollector::wrap({top.value().get(), &counter});
    ASSERT_TRUE(multi.has_value());
    ASSERT_TRUE(searcher.search_with_collector(*query, *multi.value()).has_value());

    EXPECT_EQ(counter.total_hits(), kDocs / 3);

    auto hits = top.value()->top_docs();
    ASSERT_TRUE(hits.has_value());
    ASSERT_EQ(hits.value().score_docs.size(), 2U);

    // The two collectors together answer what one of them answers alone.
    auto alone = searcher.search(*query, 2);
    ASSERT_TRUE(alone.has_value());
    EXPECT_EQ(docs_of(hits.value()), docs_of(alone.value()));
}
