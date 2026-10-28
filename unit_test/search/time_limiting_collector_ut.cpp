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

module lucene_core:ut.time_limiting_collector;

import lucene_core;

using namespace lucene;

namespace {

/// A timeout that never fires.
class NeverTimeout final : public QueryTimeout {
public:
    [[nodiscard]] bool should_exit() const override { return false; }
};

/// A timeout that fires after `remaining` checks.
class BudgetTimeout final : public QueryTimeout {
public:
    explicit BudgetTimeout(int32_t checks) : remaining_(checks) {}

    [[nodiscard]] bool should_exit() const override {
        if (remaining_ > 0) {
            --remaining_;
            return false;
        }
        return true;
    }

private:
    mutable int32_t remaining_;
};

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    return type;
}

/// An index with `count` documents in `leaf_count` segments.
std::shared_ptr<DirectoryReader> build_index(ByteBuffersDirectory &directory, int32_t count, int32_t leaf_count) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());
    auto writer = IndexWriter::open(directory, config);
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }
    const int32_t per_leaf = count / leaf_count;
    for (int32_t leaf = 0; leaf < leaf_count; ++leaf) {
        for (int32_t i = 0; i < per_leaf; ++i) {
            Document doc;
            doc.add(std::make_unique<Field>("f", FieldValue{std::string("value")}, text_type()));
            auto added = writer.value()->add_document(doc);
            EXPECT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
        }
        auto committed = writer.value()->commit();
        EXPECT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    auto reader = DirectoryReader::open(directory);
    EXPECT_TRUE(reader.has_value());
    return reader.has_value() ? reader.value() : nullptr;
}

} // namespace

TEST(TimeLimitingCollectorTest, ATimeoutThatNeverFiresLeavesTheResultAlone) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory, 300, 3);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    NeverTimeout timeout;
    auto hits = searcher.search(MatchAllDocsQuery(), 10, timeout);
    ASSERT_TRUE(hits.has_value()) << (hits ? "" : hits.error().to_string());
    EXPECT_EQ(hits.value().score_docs.size(), 10U);
    EXPECT_EQ(hits.value().total_hits.value, 300);
}

TEST(TimeLimitingCollectorTest, AnExpiredTimeoutSurfacesAsAnError) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory, 300, 3);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    // Fires on the very first check: the first leaf's first batch is rejected.
    BudgetTimeout timeout(0);
    auto hits = searcher.search(MatchAllDocsQuery(), 10, timeout);
    ASSERT_FALSE(hits.has_value());
    EXPECT_EQ(hits.error().kind(), ErrorKind::kTimeExceeded);
}

TEST(TimeLimitingCollectorTest, TheTimeoutIsCheckedEveryBatchOfDocuments) {
    ByteBuffersDirectory directory;
    // One leaf, 4096 documents: with the default 1024 document batch the timeout
    // is asked four times while the leaf is collected.
    auto reader = build_index(directory, 4096, 1);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    // Two checks pass, the third one stops the search.
    BudgetTimeout timeout(2);
    auto hits = searcher.search(MatchAllDocsQuery(), 10, timeout);
    ASSERT_FALSE(hits.has_value());
    EXPECT_EQ(hits.error().kind(), ErrorKind::kTimeExceeded);

    // A budget that covers every check returns the page.
    BudgetTimeout generous(100);
    auto page = searcher.search(MatchAllDocsQuery(), 10, generous);
    ASSERT_TRUE(page.has_value()) << (page ? "" : page.error().to_string());
    EXPECT_EQ(page.value().total_hits.value, 4096);
}

TEST(TimeLimitingCollectorTest, SearchAfterScoreAcceptsATimeout) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory, 300, 3);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    NeverTimeout timeout;
    auto first = searcher.search_after_score(nullptr, MatchAllDocsQuery(), 5, timeout);
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
    ASSERT_EQ(first.value().score_docs.size(), 5U);

    ScoreDoc cursor = first.value().score_docs.back();
    auto second = searcher.search_after_score(&cursor, MatchAllDocsQuery(), 5, timeout);
    ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());
    EXPECT_EQ(second.value().score_docs.size(), 5U);
    EXPECT_GT(second.value().score_docs.front().doc, cursor.doc);

    BudgetTimeout expired(0);
    auto stopped = searcher.search_after_score(nullptr, MatchAllDocsQuery(), 5, expired);
    ASSERT_FALSE(stopped.has_value());
    EXPECT_EQ(stopped.error().kind(), ErrorKind::kTimeExceeded);
}

TEST(TimeLimitingCollectorTest, RejectsANonPositivePageSize) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory, 10, 1);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    NeverTimeout timeout;
    EXPECT_FALSE(searcher.search(MatchAllDocsQuery(), 0, timeout).has_value());
    EXPECT_FALSE(searcher.search_after_score(nullptr, MatchAllDocsQuery(), -1, timeout).has_value());
}
