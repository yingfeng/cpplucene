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
#include <string>
#include <vector>

module lucene_core:ut.bytes_ref_hash;

import lucene_core;

using namespace lucene;

namespace {

BytesRef term(std::string_view text) { return BytesRef::from_string(text); }

/// Reads the term stored under `id` back out of the pool.
std::string term_of(const BytesRefHash &hash, const ByteBlockPool &pool, int32_t id) {
    auto position = hash.get(id, pool);
    if (!position.has_value()) {
        return "<error>";
    }
    auto bytes = BytesRefBlockPool{}.bytes_ref(position.value(), pool);
    if (!bytes.has_value()) {
        return "<error>";
    }
    const auto slice = bytes.value().as_slice();
    return std::string(slice.begin(), slice.end());
}

} // namespace

TEST(BytesRefHashTest, AddsNewTermsAndFindsExistingOnes) {
    ByteBlockPool pool;
    BytesRefHash hash;

    // New terms get 0, 1, 2...
    auto first = hash.add(term("alpha"), pool);
    ASSERT_TRUE(first.has_value()) << first.error().to_string();
    EXPECT_EQ(first.value(), 0);
    auto second = hash.add(term("bravo"), pool);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value(), 1);
    EXPECT_EQ(hash.size(), 2U);

    // ... and an existing term comes back as -(id + 1).
    auto again = hash.add(term("alpha"), pool);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again.value(), -1);
    EXPECT_EQ(hash.size(), 2U);

    EXPECT_EQ(hash.find(term("alpha"), pool).value(), 0);
    EXPECT_EQ(hash.find(term("bravo"), pool).value(), 1);
    EXPECT_EQ(hash.find(term("missing"), pool).value(), -1);

    EXPECT_EQ(term_of(hash, pool, 0), "alpha");
    EXPECT_EQ(term_of(hash, pool, 1), "bravo");
    EXPECT_EQ(hash.byte_start(0).value() >= 0, true);
}

TEST(BytesRefHashTest, GrowsAndRehashesUnderLoad) {
    ByteBlockPool pool;
    BytesRefHash hash;

    constexpr int32_t kCount = 2000;
    for (int32_t i = 0; i < kCount; ++i) {
        auto added = hash.add(term("term-" + std::to_string(i)), pool);
        ASSERT_TRUE(added.has_value()) << added.error().to_string();
        EXPECT_EQ(added.value(), i);
    }
    ASSERT_EQ(hash.size(), static_cast<size_t>(kCount));

    // Every term must still be findable after the rehashes.
    for (int32_t i = 0; i < kCount; ++i) {
        auto found = hash.find(term("term-" + std::to_string(i)), pool);
        ASSERT_TRUE(found.has_value()) << found.error().to_string();
        EXPECT_EQ(found.value(), i);
    }
    EXPECT_EQ(hash.find(term("term-2000"), pool).value(), -1);
    EXPECT_EQ(term_of(hash, pool, 1234), "term-1234");
}

TEST(BytesRefHashTest, SortsTermIdsByTermBytes) {
    ByteBlockPool pool;
    BytesRefHash hash;

    const std::vector<std::string> terms = {"delta", "alpha", "charlie", "bravo", "echo", "alpha2"};
    for (const auto &text : terms) {
        ASSERT_TRUE(hash.add(term(text), pool).has_value());
    }

    ASSERT_TRUE(hash.sort(pool).has_value());

    std::vector<std::string> sorted_terms;
    for (size_t i = 0; i < hash.size(); ++i) {
        sorted_terms.push_back(term_of(hash, pool, hash.ids()[i]));
    }
    std::vector<std::string> expected = terms;
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(sorted_terms, expected);

    // Sorting must not lose or duplicate ids.
    std::vector<int32_t> ids(hash.ids().begin(), hash.ids().begin() + static_cast<std::ptrdiff_t>(hash.size()));
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<int32_t>{0, 1, 2, 3, 4, 5}));
}

TEST(BytesRefHashTest, SortsWithSharedPrefixesAndShortTerms) {
    ByteBlockPool pool;
    BytesRefHash hash;

    // One key is a strict prefix of the others, which exercises the "-1" bucket.
    std::vector<std::string> terms = {"prefix", "prefix-one", "prefix-two", "prefix-two-a", "prefix~"};
    for (int32_t i = 0; i < 200; ++i) {
        terms.push_back("prefix-" + std::to_string(i % 40));
    }
    for (const auto &text : terms) {
        ASSERT_TRUE(hash.add(term(text), pool).has_value());
    }

    ASSERT_TRUE(hash.sort(pool).has_value());

    std::vector<std::string> sorted_terms;
    for (size_t i = 0; i < hash.size(); ++i) {
        sorted_terms.push_back(term_of(hash, pool, hash.ids()[i]));
    }
    // Repeated terms collapse onto one id, so compare against the deduplicated set.
    std::vector<std::string> expected = terms;
    std::sort(expected.begin(), expected.end());
    expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
    EXPECT_EQ(sorted_terms, expected);
}

TEST(BytesRefHashTest, ClearsAndReusesTheSameStorage) {
    ByteBlockPool pool;
    BytesRefHash hash;

    ASSERT_TRUE(hash.add(term("alpha"), pool).has_value());
    ASSERT_TRUE(hash.add(term("bravo"), pool).has_value());
    hash.clear(pool);
    // `clear` releases the `bytes_start` table; `reinit` allocates it again
    // (same contract as Lucene/the reference implementation).
    ASSERT_TRUE(hash.reinit().has_value());

    EXPECT_EQ(hash.size(), 0U);
    // The ids restart from zero, and the terms are re-stored in the reset pool.
    EXPECT_EQ(hash.add(term("charlie"), pool).value(), 0);
    EXPECT_EQ(hash.find(term("charlie"), pool).value(), 0);
    EXPECT_EQ(hash.find(term("alpha"), pool).value(), -1);
    EXPECT_EQ(term_of(hash, pool, 0), "charlie");
}

TEST(BytesRefHashTest, AddsByPoolOffset) {
    ByteBlockPool pool;
    BytesRefHash hash;

    // Terms that share the same bytes hash to the same slot; offsets must not.
    auto first = hash.add_by_pool_offset(100, pool);
    ASSERT_TRUE(first.has_value()) << first.error().to_string();
    EXPECT_EQ(first.value(), 0);
    auto second = hash.add_by_pool_offset(200, pool);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value(), 1);
    auto again = hash.add_by_pool_offset(100, pool);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again.value(), -1);
    EXPECT_EQ(hash.size(), 2U);
    EXPECT_EQ(hash.byte_start(0).value(), 100);
    EXPECT_EQ(hash.byte_start(1).value(), 200);
}

TEST(BytesRefHashTest, RejectsOversizedTerms) {
    ByteBlockPool pool;
    BytesRefHash hash;
    auto added = hash.add(term(std::string(BYTE_BLOCK_SIZE, 'x')), pool);
    ASSERT_FALSE(added.has_value());
    EXPECT_EQ(added.error().kind(), ErrorKind::kMaxBytesLengthExceeded);
    EXPECT_EQ(hash.size(), 0U);
}
