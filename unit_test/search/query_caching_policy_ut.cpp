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

module lucene_core:ut.query_caching_policy;

import lucene_core;

using namespace lucene;

namespace {

std::shared_ptr<BooleanQuery> make_boolean_query() {
    BooleanQuery::Builder builder;
    EXPECT_TRUE(builder.add(std::make_shared<PrefixQuery>("body", "al"), Occur::Should).has_value());
    EXPECT_TRUE(builder.add(std::make_shared<PrefixQuery>("body", "be"), Occur::Should).has_value());
    auto query = builder.build();
    EXPECT_TRUE(query.has_value());
    return std::make_shared<BooleanQuery>(std::move(query.value()));
}

} // namespace

TEST(QueryCachingPolicyTest, RejectsTooSmallHistory) {
    auto policy = UsageTrackingQueryCachingPolicy::with_history_size(1);
    ASSERT_FALSE(policy.has_value());
    EXPECT_EQ(policy.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(QueryCachingPolicyTest, NeverCachesCheapQueries) {
    auto created = UsageTrackingQueryCachingPolicy::create();
    ASSERT_TRUE(created.has_value()) << created.error().to_string();
    const auto &policy = *created.value();

    const TermQuery term_query(Term::from_text("body", "alpha"));
    const MatchAllDocsQuery match_all;
    const MatchNoDocsQuery match_none;

    for (int32_t i = 0; i < 10; ++i) {
        policy.on_use(term_query);
        policy.on_use(match_all);
        policy.on_use(match_none);
    }
    EXPECT_FALSE(policy.should_cache(term_query).value());
    EXPECT_FALSE(policy.should_cache(match_all).value());
    EXPECT_FALSE(policy.should_cache(match_none).value());
    EXPECT_EQ(policy.frequency(term_query), 0);
}

TEST(QueryCachingPolicyTest, CachesCostlyQueriesAfterTwoUses) {
    auto created = UsageTrackingQueryCachingPolicy::create();
    ASSERT_TRUE(created.has_value());
    const auto &policy = *created.value();

    // A multi-term query expands a whole term dictionary, so it is worth caching
    // early.
    const PrefixQuery prefix("body", "al");
    EXPECT_EQ(UsageTrackingQueryCachingPolicy::min_frequency_to_cache(prefix), 2);
    EXPECT_TRUE(UsageTrackingQueryCachingPolicy::is_costly(prefix));

    policy.on_use(prefix);
    EXPECT_FALSE(policy.should_cache(prefix).value());
    EXPECT_EQ(policy.frequency(prefix), 1);

    policy.on_use(prefix);
    EXPECT_TRUE(policy.should_cache(prefix).value());
    EXPECT_EQ(policy.frequency(prefix), 2);
}

TEST(QueryCachingPolicyTest, CachesCompoundQueriesAfterFourUses) {
    auto created = UsageTrackingQueryCachingPolicy::create();
    ASSERT_TRUE(created.has_value());
    const auto &policy = *created.value();

    auto query = make_boolean_query();
    EXPECT_EQ(UsageTrackingQueryCachingPolicy::min_frequency_to_cache(*query), 4);
    EXPECT_FALSE(UsageTrackingQueryCachingPolicy::is_costly(*query));

    for (int32_t i = 0; i < 3; ++i) {
        policy.on_use(*query);
    }
    EXPECT_FALSE(policy.should_cache(*query).value());

    policy.on_use(*query);
    EXPECT_TRUE(policy.should_cache(*query).value());
    EXPECT_EQ(policy.frequency(*query), 4);
}

TEST(QueryCachingPolicyTest, NeverCachesAnEmptyBooleanQuery) {
    auto created = UsageTrackingQueryCachingPolicy::create();
    ASSERT_TRUE(created.has_value());
    const auto &policy = *created.value();

    BooleanQuery::Builder builder;
    auto built = builder.build();
    ASSERT_TRUE(built.has_value());
    const BooleanQuery empty(std::move(built.value()));

    for (int32_t i = 0; i < 10; ++i) {
        policy.on_use(empty);
    }
    EXPECT_FALSE(policy.should_cache(empty).value());
    EXPECT_EQ(policy.frequency(empty), 0);
}

TEST(QueryCachingPolicyTest, TracksEqualityByHashCode) {
    auto created = UsageTrackingQueryCachingPolicy::create();
    ASSERT_TRUE(created.has_value());
    const auto &policy = *created.value();

    // Two separate instances of the same query share a hash code, which is what
    // the policy tracks.
    const PrefixQuery first("body", "al");
    const PrefixQuery second("body", "al");
    ASSERT_EQ(first.hash(), second.hash());
    policy.on_use(first);
    policy.on_use(second);
    EXPECT_TRUE(policy.should_cache(second).value());

    // A different prefix is tracked separately.
    const PrefixQuery other("body", "be");
    EXPECT_FALSE(policy.should_cache(other).value());
}
