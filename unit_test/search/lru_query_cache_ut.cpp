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
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.lru_query_cache;

import lucene_core;

using namespace lucene;

namespace {

/// A policy that caches everything, so that the cache itself can be tested
/// without waiting for a query to become popular.
class AlwaysCachePolicy final : public QueryCachingPolicy {
public:
    void on_use(const Query & /*query*/) const override { ++uses_; }

    [[nodiscard]] Result<bool> should_cache(const Query & /*query*/) const override { return true; }

    [[nodiscard]] int32_t uses() const noexcept { return uses_; }

private:
    mutable int32_t uses_{0};
};

/// A leaf reader with `max_doc` documents and no postings, which is enough for a
/// match-all query.
class CacheLeafReader final : public LeafReader {
public:
    explicit CacheLeafReader(int32_t max_doc) : max_doc_(max_doc) {}

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return max_doc_; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override {
        return std::unique_ptr<StoredFields>{};
    }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override {
        return std::unique_ptr<TermVectors>{};
    }
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view /*field*/) override {
        return std::unique_ptr<Terms>{};
    }

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(
            std::string_view /*field*/) override {
        return std::unique_ptr<NumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<BinaryDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<SortedDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>>
    get_sorted_numeric_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<SortedNumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>>
    get_sorted_set_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<SortedSetDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view /*field*/) override {
        return std::unique_ptr<NumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(
            std::string_view /*field*/) override {
        return std::unique_ptr<DocValuesSkipper>{};
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    int32_t max_doc_;
    FieldInfos field_infos_{FieldInfos::none()};
};

/// Answers the documents the weight matches on the first leaf.
std::vector<int32_t> collect_docs(Weight &weight, const LeafReaderContext &context, const IndexSearcher &searcher) {
    std::vector<int32_t> docs;
    auto supplier = weight.scorer_supplier(context, searcher);
    if (!supplier.has_value() || supplier.value() == nullptr) {
        return docs;
    }
    auto scorer = supplier.value()->get(1, context, searcher);
    if (!scorer.has_value() || scorer.value() == nullptr) {
        return docs;
    }
    while (true) {
        auto doc = scorer.value()->next_doc();
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        docs.push_back(doc.value());
    }
    return docs;
}

} // namespace

TEST(LRUQueryCacheTest, RejectsInvalidLimits) {
    EXPECT_FALSE(LRUQueryCache::create(0, 1024).has_value());
    EXPECT_FALSE(LRUQueryCache::create(2, 0).has_value());
    EXPECT_FALSE(LRUQueryCache::with_max_size(-1).has_value());
    EXPECT_TRUE(LRUQueryCache::create(2, 1024).has_value());
}

TEST(LRUQueryCacheTest, CachesTheDocumentsOfALeaf) {
    auto cache = LRUQueryCache::create(4, 1 << 20);
    ASSERT_TRUE(cache.has_value()) << cache.error().to_string();
    auto policy = std::make_shared<AlwaysCachePolicy>();

    IndexSearcher searcher(std::make_shared<CacheLeafReader>(4));
    searcher.set_query_cache(cache.value());
    searcher.set_caching_policy(policy);

    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());
    const LeafReaderContext &context = leaves.value()[0];

    auto query = std::make_shared<MatchAllDocsQuery>();
    auto weight = searcher.create_weight(*query, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(weight.has_value()) << weight.error().to_string();
    EXPECT_EQ(collect_docs(*weight.value(), context, searcher), (std::vector<int32_t>{0, 1, 2, 3}));
    EXPECT_EQ(cache.value()->miss_count(), 1);
    EXPECT_EQ(cache.value()->hit_count(), 0);
    EXPECT_EQ(cache.value()->cache_count(), 1);
    EXPECT_EQ(cache.value()->cache_size(), 1);
    EXPECT_GT(cache.value()->ram_bytes_used(), 0);

    // The same query again is answered from the cache.
    auto second = searcher.create_weight(*query, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(collect_docs(*second.value(), context, searcher), (std::vector<int32_t>{0, 1, 2, 3}));
    EXPECT_EQ(cache.value()->hit_count(), 1);
    EXPECT_EQ(cache.value()->miss_count(), 1);

    // An equal, distinct query instance hits the same entry.
    auto equal = std::make_shared<MatchAllDocsQuery>();
    auto third = searcher.create_weight(*equal, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(collect_docs(*third.value(), context, searcher), (std::vector<int32_t>{0, 1, 2, 3}));
    EXPECT_EQ(cache.value()->hit_count(), 2);
    EXPECT_EQ(cache.value()->cache_size(), 1);
}

TEST(LRUQueryCacheTest, DoesNotCacheWhenScoresAreNeeded) {
    auto cache = LRUQueryCache::create(4, 1 << 20);
    ASSERT_TRUE(cache.has_value());
    auto policy = std::make_shared<AlwaysCachePolicy>();

    IndexSearcher searcher(std::make_shared<CacheLeafReader>(3));
    searcher.set_query_cache(cache.value());
    searcher.set_caching_policy(policy);

    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    auto query = std::make_shared<MatchAllDocsQuery>();
    // `TopScores` needs scores, which a cached document set cannot provide.
    auto weight = searcher.create_weight(*query, ScoreMode::TopScores, 1.0F);
    ASSERT_TRUE(weight.has_value()) << weight.error().to_string();
    EXPECT_EQ(collect_docs(*weight.value(), leaves.value()[0], searcher), (std::vector<int32_t>{0, 1, 2}));
    EXPECT_EQ(cache.value()->cache_size(), 0);
    EXPECT_EQ(cache.value()->miss_count(), 0);
}

TEST(LRUQueryCacheTest, DoesNotCacheWithoutAPolicy) {
    auto cache = LRUQueryCache::create(4, 1 << 20);
    ASSERT_TRUE(cache.has_value());

    IndexSearcher searcher(std::make_shared<CacheLeafReader>(2));
    searcher.set_query_cache(cache.value());

    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    auto query = std::make_shared<MatchAllDocsQuery>();
    auto weight = searcher.create_weight(*query, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(weight.has_value());
    EXPECT_EQ(collect_docs(*weight.value(), leaves.value()[0], searcher), (std::vector<int32_t>{0, 1}));
    EXPECT_EQ(cache.value()->cache_size(), 0);
}

TEST(LRUQueryCacheTest, EvictsTheLeastRecentlyUsedEntry) {
    auto cache = LRUQueryCache::with_max_size(1);
    ASSERT_TRUE(cache.has_value()) << cache.error().to_string();
    auto policy = std::make_shared<AlwaysCachePolicy>();
    auto query = std::make_shared<MatchAllDocsQuery>();

    // One entry per reader, so two readers with the same query evict each other
    // in a cache that holds a single entry.
    IndexSearcher first_searcher(std::make_shared<CacheLeafReader>(2));
    first_searcher.set_query_cache(cache.value());
    first_searcher.set_caching_policy(policy);
    auto first_leaves = first_searcher.leaves();
    ASSERT_TRUE(first_leaves.has_value());
    auto first_weight = first_searcher.create_weight(*query, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(first_weight.has_value());
    collect_docs(*first_weight.value(), first_leaves.value()[0], first_searcher);
    EXPECT_EQ(cache.value()->cache_size(), 1);

    IndexSearcher second_searcher(std::make_shared<CacheLeafReader>(3));
    second_searcher.set_query_cache(cache.value());
    second_searcher.set_caching_policy(policy);
    auto second_leaves = second_searcher.leaves();
    ASSERT_TRUE(second_leaves.has_value());
    auto second_weight = second_searcher.create_weight(*query, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(second_weight.has_value());
    collect_docs(*second_weight.value(), second_leaves.value()[0], second_searcher);

    EXPECT_EQ(cache.value()->cache_size(), 1);
    EXPECT_EQ(cache.value()->eviction_count(), 1);

    // Clearing drops everything.
    cache.value()->clear();
    EXPECT_EQ(cache.value()->cache_size(), 0);
    EXPECT_EQ(cache.value()->ram_bytes_used(), 0);
}

TEST(LRUQueryCacheTest, UsageTrackingPolicyCachesOnlyPopularQueries) {
    auto cache = LRUQueryCache::create(4, 1 << 20);
    ASSERT_TRUE(cache.has_value());
    auto policy = UsageTrackingQueryCachingPolicy::create();
    ASSERT_TRUE(policy.has_value()) << policy.error().to_string();
    std::shared_ptr<const QueryCachingPolicy> shared_policy(std::move(policy.value()));

    IndexSearcher searcher(std::make_shared<CacheLeafReader>(2));
    searcher.set_query_cache(cache.value());
    searcher.set_caching_policy(shared_policy);

    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    // A multi-term query becomes worth caching on its second use.
    auto query = std::make_shared<PrefixQuery>("body", "al");
    auto weight = searcher.create_weight(*query, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(weight.has_value());
    collect_docs(*weight.value(), leaves.value()[0], searcher);
    EXPECT_EQ(cache.value()->cache_size(), 0);

    auto second = searcher.create_weight(*query, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(second.has_value());
    collect_docs(*second.value(), leaves.value()[0], searcher);
    EXPECT_EQ(cache.value()->cache_size(), 1);
}
