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
#include <vector>

module lucene_core:ut.doc_id_set_impls;

import lucene_core;

using namespace lucene;

namespace {

/// Collects every document an iterator visits.
std::vector<int32_t> drain(DocIdSetIterator &iterator) {
    std::vector<int32_t> docs;
    while (true) {
        auto doc = iterator.next_doc();
        EXPECT_TRUE(doc.has_value()) << (doc ? "" : doc.error().to_string());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        docs.push_back(doc.value());
    }
    return docs;
}

} // namespace

TEST(IntArrayDocIdSetTest, IteratesAndAdvances) {
    auto set = IntArrayDocIdSet::create({1, 3, 5, NO_MORE_DOCS}, 3);
    ASSERT_TRUE(set.has_value()) << (set ? "" : set.error().to_string());
    EXPECT_EQ(set.value()->length(), 3U);
    EXPECT_EQ(set.value()->docs(), (std::vector<int32_t>{1, 3, 5, NO_MORE_DOCS}));
    EXPECT_EQ(set.value()->bits().value(), nullptr);

    auto iterator = set.value()->iterator();
    ASSERT_TRUE(iterator.has_value());
    EXPECT_EQ(iterator.value()->doc_id(), -1);
    EXPECT_EQ(iterator.value()->cost().value(), 3);
    EXPECT_EQ(drain(*iterator.value()), (std::vector<int32_t>{1, 3, 5}));
    EXPECT_EQ(iterator.value()->doc_id(), NO_MORE_DOCS);

    auto advancing = set.value()->iterator();
    ASSERT_TRUE(advancing.has_value());
    EXPECT_EQ(advancing.value()->advance(4).value(), 5);
    EXPECT_EQ(advancing.value()->advance(6).value(), NO_MORE_DOCS);
    // Staying exhausted is allowed (the reference implementation over-reads here).
    EXPECT_EQ(advancing.value()->advance(7).value(), NO_MORE_DOCS);

    auto from_start = set.value()->iterator();
    ASSERT_TRUE(from_start.has_value());
    EXPECT_EQ(from_start.value()->advance(0).value(), 1);
    EXPECT_EQ(from_start.value()->advance(2).value(), 3);
}

TEST(IntArrayDocIdSetTest, RejectsBadArrays) {
    // Missing NO_MORE_DOCS sentinel at `length`.
    EXPECT_FALSE(IntArrayDocIdSet::create({1, 2, 3}, 2).has_value());
    // `length` past the end.
    EXPECT_FALSE(IntArrayDocIdSet::create({1, NO_MORE_DOCS}, 5).has_value());
    // Not sorted.
    EXPECT_FALSE(IntArrayDocIdSet::create({3, 1, NO_MORE_DOCS}, 2).has_value());
    // Empty set is fine: docs[0] is the sentinel.
    auto empty = IntArrayDocIdSet::create({NO_MORE_DOCS}, 0);
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(drain(*empty.value()->iterator().value()).empty());
}

TEST(NotDocIdSetTest, IteratesTheComplement) {
    auto inner = IntArrayDocIdSet::create({1, 3, NO_MORE_DOCS}, 2);
    ASSERT_TRUE(inner.has_value());
    NotDocIdSet set(6, std::shared_ptr<DocIdSet>(inner.value()));

    auto iterator = set.iterator();
    ASSERT_TRUE(iterator.has_value());
    EXPECT_EQ(iterator.value()->cost().value(), 6);
    EXPECT_EQ(drain(*iterator.value()), (std::vector<int32_t>{0, 2, 4, 5}));

    auto advancing = set.iterator();
    ASSERT_TRUE(advancing.has_value());
    EXPECT_EQ(advancing.value()->advance(2).value(), 2);
    EXPECT_EQ(advancing.value()->advance(3).value(), 4);
    EXPECT_EQ(advancing.value()->advance(6).value(), NO_MORE_DOCS);

    // No random access: the wrapped set has none either.
    EXPECT_EQ(set.bits().value(), nullptr);
}

TEST(NotDocIdSetTest, ComplementBitsWhenTheWrappedSetHasThem) {
    auto bits = std::make_shared<FixedBitSet>(6);
    bits->set(1);
    bits->set(3);
    auto inner = BitDocIdSet::create(bits);
    ASSERT_TRUE(inner.has_value());
    NotDocIdSet set(6, std::shared_ptr<DocIdSet>(inner.value()));

    auto complement = set.bits();
    ASSERT_TRUE(complement.has_value());
    ASSERT_NE(complement.value(), nullptr);
    EXPECT_EQ(complement.value()->length(), 6U);
    EXPECT_TRUE(complement.value()->get(0).value());
    EXPECT_FALSE(complement.value()->get(1).value());
    EXPECT_TRUE(complement.value()->get(2).value());
    EXPECT_FALSE(complement.value()->get(3).value());
}

TEST(BitDocIdSetTest, IteratesAndEstimatesCost) {
    auto bits = std::make_shared<FixedBitSet>(8);
    bits->set(0);
    bits->set(2);
    bits->set(5);

    auto set = BitDocIdSet::create(bits);
    ASSERT_TRUE(set.has_value()) << (set ? "" : set.error().to_string());
    // The cardinality of a FixedBitSet is the cost.
    EXPECT_EQ(set.value()->cost(), 3);
    EXPECT_EQ(set.value()->bits().value(), bits);

    auto iterator = set.value()->iterator();
    ASSERT_TRUE(iterator.has_value());
    EXPECT_EQ(iterator.value()->cost().value(), 3);
    EXPECT_EQ(drain(*iterator.value()), (std::vector<int32_t>{0, 2, 5}));

    auto advancing = set.value()->iterator();
    ASSERT_TRUE(advancing.has_value());
    EXPECT_EQ(advancing.value()->advance(1).value(), 2);
    EXPECT_EQ(advancing.value()->advance(3).value(), 5);
    EXPECT_EQ(advancing.value()->advance(8).value(), NO_MORE_DOCS);

    auto explicit_cost = BitDocIdSet::create_with_cost(bits, 42);
    ASSERT_TRUE(explicit_cost.has_value());
    EXPECT_EQ(explicit_cost.value()->cost(), 42);
    EXPECT_FALSE(BitDocIdSet::create_with_cost(bits, -1).has_value());
    EXPECT_FALSE(BitDocIdSet::create(nullptr).has_value());
}

TEST(BitDocIdSetTest, NonFixedBitsUseTheLengthAsCost) {
    // MatchAllBits is a Bits but not a FixedBitSet.
    auto all = std::make_shared<MatchAllBits>(4);
    auto set = BitDocIdSet::create(all);
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(set.value()->cost(), 4);
    EXPECT_EQ(drain(*set.value()->iterator().value()), (std::vector<int32_t>{0, 1, 2, 3}));
}
