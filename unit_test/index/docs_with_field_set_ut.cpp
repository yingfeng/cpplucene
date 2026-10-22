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
#include <vector>

module lucene_core:ut.docs_with_field_set;

import lucene_core;

using namespace lucene;

namespace {

std::vector<int32_t> collect(const DocIdSet &set) {
    std::vector<int32_t> docs;
    auto iterator = set.iterator();
    EXPECT_TRUE(iterator.has_value());
    while (true) {
        auto doc = iterator.value()->next_doc();
        EXPECT_TRUE(doc.has_value());
        if (doc.value() == NO_MORE_DOCS) {
            break;
        }
        docs.push_back(doc.value());
    }
    return docs;
}

} // namespace

TEST(DocsWithFieldSetTest, ContiguousDocumentsStayAnAllIterator) {
    DocsWithFieldSet set;
    for (int32_t doc = 0; doc < 4; ++doc) {
        ASSERT_TRUE(set.add(doc).has_value());
    }
    EXPECT_EQ(set.cardinality(), 4);
    ASSERT_TRUE(set.finish().has_value());
    EXPECT_EQ(collect(set), (std::vector<int32_t>{0, 1, 2, 3}));
}

TEST(DocsWithFieldSetTest, GapsMaterializeABitSet) {
    DocsWithFieldSet set;
    ASSERT_TRUE(set.add(0).has_value());
    ASSERT_TRUE(set.add(2).has_value());
    ASSERT_TRUE(set.add(5).has_value());
    ASSERT_TRUE(set.add(9).has_value());
    EXPECT_EQ(set.cardinality(), 4);
    ASSERT_TRUE(set.finish().has_value());

    auto docs = collect(set);
    EXPECT_EQ(docs, (std::vector<int32_t>{0, 2, 5, 9}));

    // `advance` skips forward like any doc-id-set iterator.
    auto iterator = set.iterator();
    ASSERT_TRUE(iterator.has_value());
    EXPECT_EQ(iterator.value()->advance(3).value(), 5);
    EXPECT_EQ(iterator.value()->advance(9).value(), 9);
    EXPECT_EQ(iterator.value()->next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(iterator.value()->cost().value(), 4);
}

TEST(DocsWithFieldSetTest, RejectsOutOfOrderDocuments) {
    DocsWithFieldSet set;
    ASSERT_TRUE(set.add(3).has_value());
    auto duplicate = set.add(3);
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_FALSE(set.add(2).has_value());
}

TEST(DocsWithFieldSetTest, RejectsChangesAfterFinish) {
    DocsWithFieldSet set;
    ASSERT_TRUE(set.add(0).has_value());
    ASSERT_TRUE(set.finish().has_value());
    auto added = set.add(1);
    ASSERT_FALSE(added.has_value());
    EXPECT_EQ(added.error().kind(), ErrorKind::kIllegalState);
}

TEST(DocsWithFieldSetTest, IteratorRequiresFinish) {
    DocsWithFieldSet set;
    ASSERT_TRUE(set.add(0).has_value());
    auto iterator = set.iterator();
    ASSERT_FALSE(iterator.has_value());
    EXPECT_EQ(iterator.error().kind(), ErrorKind::kIllegalState);
}

TEST(DocsWithFieldSetTest, EmptySetIteratesNothing) {
    DocsWithFieldSet set;
    ASSERT_TRUE(set.finish().has_value());
    EXPECT_EQ(set.cardinality(), 0);
    EXPECT_TRUE(collect(set).empty());
}
