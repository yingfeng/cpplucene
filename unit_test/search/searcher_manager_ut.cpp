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
#include <string>
#include <utility>

module lucene_core:ut.searcher_manager;

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

void add_title(IndexWriter &writer, std::string title) {
    Document doc;
    doc.add(std::make_unique<Field>("title", FieldValue{std::move(title)}, text_type()));
    auto added = writer.add_document(doc);
    EXPECT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
}

/// A factory that hands out BM25 searchers and counts the calls.
class CountingSearcherFactory final : public SearcherFactory {
public:
    [[nodiscard]] Result<std::shared_ptr<IndexSearcher>> new_searcher(
            std::shared_ptr<IndexReader> reader) const override {
        ++calls;
        return std::make_shared<IndexSearcher>(std::move(reader));
    }

    mutable int32_t calls{0};
};

} // namespace

TEST(SearcherManagerTest, AcquireReturnsTheCurrentSearcher) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());
    add_title(*writer.value(), "hello");
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto manager = SearcherManager::open(directory);
    ASSERT_TRUE(manager.has_value()) << (manager ? "" : manager.error().to_string());

    auto searcher = manager.value()->acquire();
    ASSERT_NE(searcher, nullptr);
    auto count = searcher->count(MatchAllDocsQuery());
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 1);

    // Nothing changed, so a refresh reports no work.
    auto refreshed = manager.value()->maybe_refresh();
    ASSERT_TRUE(refreshed.has_value()) << (refreshed ? "" : refreshed.error().to_string());
    EXPECT_FALSE(refreshed.value());
}

TEST(SearcherManagerTest, RefreshPicksUpANewCommit) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());
    add_title(*writer.value(), "hello");
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto manager = SearcherManager::open(directory);
    ASSERT_TRUE(manager.has_value());
    auto before = manager.value()->acquire();

    add_title(*writer.value(), "world");
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto refreshed = manager.value()->maybe_refresh();
    ASSERT_TRUE(refreshed.has_value()) << (refreshed ? "" : refreshed.error().to_string());
    EXPECT_TRUE(refreshed.value());

    auto after = manager.value()->acquire();
    ASSERT_NE(after, nullptr);
    auto count = after->count(MatchAllDocsQuery());
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 2);

    // The searcher handed out before the refresh still sees the old commit: that
    // is what lets a running query finish.
    auto old_count = before->count(MatchAllDocsQuery());
    ASSERT_TRUE(old_count.has_value());
    EXPECT_EQ(old_count.value(), 1);
}

TEST(SearcherManagerTest, TheFactoryBuildsEverySearcher) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());
    add_title(*writer.value(), "hello");
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto factory = std::make_shared<CountingSearcherFactory>();
    auto manager = SearcherManager::open(directory, factory);
    ASSERT_TRUE(manager.has_value());
    EXPECT_EQ(factory->calls, 1);

    add_title(*writer.value(), "world");
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(manager.value()->maybe_refresh().value());
    EXPECT_EQ(factory->calls, 2);
}

TEST(SearcherManagerTest, AClosedManagerRefusesToRefresh) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());
    add_title(*writer.value(), "hello");
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto manager = SearcherManager::open(directory);
    ASSERT_TRUE(manager.has_value());
    ASSERT_TRUE(manager.value()->close().has_value());
    EXPECT_TRUE(manager.value()->is_closed());

    auto refreshed = manager.value()->maybe_refresh();
    ASSERT_FALSE(refreshed.has_value());
    EXPECT_NE(refreshed.error().to_string().find("closed"), std::string::npos);
}

TEST(SearcherManagerTest, AHandBuiltReaderNeverChanges) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());
    add_title(*writer.value(), "hello");
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    std::shared_ptr<LeafReader> leaf = reader.value()->leaf_readers().front();
    auto composite = SingleLeafCompositeReader::create(std::move(leaf));
    ASSERT_TRUE(composite.has_value()) << (composite ? "" : composite.error().to_string());
    std::shared_ptr<IndexReader> hand_built = composite.value();
    auto manager = SearcherManager::open(std::move(hand_built));
    ASSERT_TRUE(manager.has_value()) << (manager ? "" : manager.error().to_string());

    // A reader that is not a DirectoryReader is never reopened.
    auto refreshed = manager.value()->maybe_refresh();
    ASSERT_TRUE(refreshed.has_value()) << (refreshed ? "" : refreshed.error().to_string());
    EXPECT_FALSE(refreshed.value());
}

TEST(SearcherLifetimeManagerTest, RecordsAndPrunesSearchers) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());
    add_title(*writer.value(), "hello");
    ASSERT_TRUE(writer.value()->commit().has_value());

    SearcherLifetimeManager manager;
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    std::shared_ptr<IndexReader> first = reader.value();
    std::shared_ptr<IndexReader> second = reader.value();

    ASSERT_TRUE(manager.record(7, std::make_shared<IndexSearcher>(first)).has_value());
    ASSERT_TRUE(manager.record(9, std::make_shared<IndexSearcher>(second)).has_value());
    EXPECT_EQ(manager.size(), 2U);

    auto found = manager.acquire(7);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(manager.acquire(8), nullptr);

    // Keep the newest two versions worth of searchers.
    const size_t dropped = manager.prune([](int64_t version) { return version > 7; });
    EXPECT_EQ(dropped, 1U);
    EXPECT_EQ(manager.size(), 1U);
    EXPECT_EQ(manager.acquire(7), nullptr);
    EXPECT_NE(manager.acquire(9), nullptr);

    manager.close();
    EXPECT_EQ(manager.size(), 0U);
    EXPECT_EQ(manager.acquire(9), nullptr);
}

TEST(SearcherLifetimeManagerTest, RefusesToRecordWithoutASearcher) {
    SearcherLifetimeManager manager;
    auto recorded = manager.record(1, nullptr);
    ASSERT_FALSE(recorded.has_value());
    EXPECT_EQ(manager.size(), 0U);
}
