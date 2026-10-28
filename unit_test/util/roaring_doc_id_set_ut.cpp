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
#include <vector>

module lucene_core:ut.roaring_doc_id_set;

import lucene_core;

using namespace lucene;

namespace {

constexpr int32_t kBlock = 1 << 16;

std::shared_ptr<RoaringDocIdSet> build(const std::vector<int32_t> &docs, size_t max_doc) {
    RoaringDocIdSet::Builder builder(max_doc);
    for (int32_t doc : docs) {
        EXPECT_TRUE(builder.add(doc).has_value());
    }
    auto built = builder.build();
    EXPECT_TRUE(built.has_value()) << (built ? "" : built.error().to_string());
    return built.has_value() ? built.value() : nullptr;
}

std::vector<int32_t> drain(const DocIdSet &set) {
    std::vector<int32_t> docs;
    auto iterator = set.iterator();
    EXPECT_TRUE(iterator.has_value());
    if (!iterator.has_value()) {
        return docs;
    }
    while (true) {
        auto doc = iterator.value()->next_doc();
        EXPECT_TRUE(doc.has_value()) << (doc ? "" : doc.error().to_string());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        docs.push_back(doc.value());
    }
    return docs;
}

} // namespace

TEST(RoaringDocIdSetTest, EncodesASparseBlockAsAnArray) {
    std::vector<int32_t> docs{0, 3, 7, 100, 4095, 65535};
    auto set = build(docs, kBlock);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->cardinality(), docs.size());
    EXPECT_EQ(set->num_blocks(), 1U);
    EXPECT_EQ(drain(*set), docs);

    // `advance` walks the array.  Lucene's contract requires the target to be
    // greater than the current document, so every probe moves forward.
    auto iterator = set->iterator();
    ASSERT_TRUE(iterator.has_value());
    EXPECT_EQ(iterator.value()->doc_id(), -1);
    EXPECT_EQ(iterator.value()->advance(4).value(), 7);
    EXPECT_EQ(iterator.value()->advance(8).value(), 100);
    EXPECT_EQ(iterator.value()->advance(65535).value(), 65535);
    EXPECT_EQ(iterator.value()->next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(iterator.value()->doc_id(), NO_MORE_DOCS);
}

TEST(RoaringDocIdSetTest, SwitchesToABitSetPastTheArrayLimit) {
    // 4097 documents: one past the array limit, so the block becomes a bit set.
    std::vector<int32_t> docs;
    docs.reserve(4097);
    for (int32_t i = 0; i < 4097; ++i) {
        docs.push_back(i * 2);
    }
    auto set = build(docs, kBlock);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->cardinality(), docs.size());
    EXPECT_EQ(drain(*set), docs);

    auto iterator = set->iterator();
    ASSERT_TRUE(iterator.has_value());
    EXPECT_EQ(iterator.value()->advance(1001).value(), 1002);
    EXPECT_EQ(iterator.value()->cost().value(), 4097);
}

TEST(RoaringDocIdSetTest, EncodesANearlyFullBlockAsItsInverse) {
    constexpr int32_t kExcluded = 100;
    std::vector<int32_t> docs;
    docs.reserve(kBlock - kExcluded);
    for (int32_t i = 0; i < kBlock; ++i) {
        if (i % (kBlock / kExcluded) == 0) {
            continue; // 100 of them
        }
        docs.push_back(i);
    }
    auto set = build(docs, kBlock);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->cardinality(), docs.size());
    EXPECT_EQ(drain(*set), docs);

    auto iterator = set->iterator();
    ASSERT_TRUE(iterator.has_value());
    EXPECT_EQ(iterator.value()->advance(0).value(), 1);
    EXPECT_EQ(iterator.value()->advance(kBlock - 1).value(), kBlock - 1);
}

TEST(RoaringDocIdSetTest, SkipsEmptyBlocks) {
    // Three blocks, only the first and the last hold documents.
    const std::vector<int32_t> docs{1, 2, 2 * kBlock + 5, 3 * kBlock};
    auto set = build(docs, 4 * static_cast<size_t>(kBlock));
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->num_blocks(), 4U);
    EXPECT_EQ(set->cardinality(), docs.size());
    EXPECT_EQ(drain(*set), docs);

    auto iterator = set->iterator();
    ASSERT_TRUE(iterator.has_value());
    EXPECT_EQ(iterator.value()->advance(0).value(), 1);
    // Lands in the empty second block and skips to the third.
    EXPECT_EQ(iterator.value()->advance(kBlock).value(), 2 * kBlock + 5);
    EXPECT_EQ(iterator.value()->advance(kBlock + 1).value(), 2 * kBlock + 5);
    EXPECT_EQ(iterator.value()->advance(3 * kBlock).value(), 3 * kBlock);
    EXPECT_EQ(iterator.value()->advance(3 * kBlock + 1).value(), NO_MORE_DOCS);
}

TEST(RoaringDocIdSetTest, RejectsOutOfOrderDocuments) {
    RoaringDocIdSet::Builder builder(kBlock);
    ASSERT_TRUE(builder.add(5).has_value());
    ASSERT_TRUE(builder.add(6).has_value());
    auto out_of_order = builder.add(6);
    ASSERT_FALSE(out_of_order.has_value());
    EXPECT_EQ(out_of_order.error().kind(), ErrorKind::kIllegalArgument);
    // The builder still works after the rejection.
    ASSERT_TRUE(builder.add(7).has_value());
    auto set = builder.build();
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(drain(*set.value()), (std::vector<int32_t>{5, 6, 7}));
}

TEST(RoaringDocIdSetTest, AddsEveryDocumentOfAnIterator) {
    // A source set whose documents are copied into a second set.
    const std::vector<int32_t> docs{0, 9, kBlock, kBlock + 1};
    auto source = build(docs, 2 * static_cast<size_t>(kBlock));
    ASSERT_NE(source, nullptr);
    auto source_iterator = source->iterator();
    ASSERT_TRUE(source_iterator.has_value());

    RoaringDocIdSet::Builder builder(2 * static_cast<size_t>(kBlock));
    ASSERT_TRUE(builder.add_disi(*source_iterator.value()).has_value());
    auto copy = builder.build();
    ASSERT_TRUE(copy.has_value());
    EXPECT_EQ(copy.value()->cardinality(), docs.size());
    EXPECT_EQ(drain(*copy.value()), docs);
}
