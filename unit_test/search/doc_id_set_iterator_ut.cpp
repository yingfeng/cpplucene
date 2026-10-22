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

module lucene_core:ut.doc_id_set_iterator;

import lucene_core;

using namespace lucene;

namespace {

/// An iterator over every doc id, used to exercise the default
/// `slow_advance` implementation.
class EveryDocDISI : public DocIdSetIterator {
public:
    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return ++doc_; }
    [[nodiscard]] Result<int64_t> cost() const override { return int64_t{100}; }

private:
    int32_t doc_{-1};
};

} // namespace

TEST(DocIdSetIteratorTest, SentinelIsMaxInt) {
    EXPECT_EQ(NO_MORE_DOCS, std::numeric_limits<int32_t>::max());
}

TEST(DocIdSetIteratorTest, EmptyIteratesNothing) {
    EmptyDISI iterator;
    EXPECT_EQ(iterator.doc_id(), -1);
    EXPECT_EQ(iterator.cost().value(), 0);

    auto next = iterator.next_doc();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next.value(), NO_MORE_DOCS);
    EXPECT_EQ(iterator.doc_id(), NO_MORE_DOCS);
}

TEST(DocIdSetIteratorTest, AllIteratesEveryDoc) {
    AllDISI iterator(3);
    EXPECT_EQ(iterator.cost().value(), 3);

    std::vector<int32_t> docs;
    for (int32_t doc = iterator.next_doc().value(); doc != NO_MORE_DOCS; doc = iterator.next_doc().value()) {
        docs.push_back(doc);
    }
    EXPECT_EQ(docs, (std::vector<int32_t>{0, 1, 2}));

    AllDISI advancing(10);
    EXPECT_EQ(advancing.advance(4).value(), 4);
    EXPECT_EQ(advancing.doc_id(), 4);
    EXPECT_EQ(advancing.advance(10).value(), NO_MORE_DOCS);
}

TEST(DocIdSetIteratorTest, RangeIteratesTheWindow) {
    auto created = RangeDISI::create(2, 5);
    ASSERT_TRUE(created.has_value());
    RangeDISI iterator = std::move(created.value());

    EXPECT_EQ(iterator.cost().value(), 3);
    EXPECT_EQ(iterator.next_doc().value(), 2);
    EXPECT_EQ(iterator.next_doc().value(), 3);
    EXPECT_EQ(iterator.next_doc().value(), 4);
    EXPECT_EQ(iterator.next_doc().value(), NO_MORE_DOCS);

    auto below = RangeDISI::create(2, 5);
    ASSERT_TRUE(below.has_value());
    EXPECT_EQ(below->advance(0).value(), 2);
    EXPECT_EQ(below->advance(3).value(), 3);
    EXPECT_EQ(below->advance(9).value(), NO_MORE_DOCS);
}

TEST(DocIdSetIteratorTest, RangeRejectsInvalidBounds) {
    auto equal = RangeDISI::create(4, 4);
    ASSERT_FALSE(equal.has_value());
    EXPECT_EQ(equal.error().kind(), ErrorKind::kIllegalArgument);

    auto negative = RangeDISI::create(-1, 4);
    ASSERT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(DocIdSetIteratorTest, SlowAdvanceWalksNextDoc) {
    EveryDocDISI iterator;
    EXPECT_EQ(iterator.slow_advance(5).value(), 5);
    EXPECT_EQ(iterator.doc_id(), 5);
}

TEST(DocIdSetIteratorTest, DefaultAdvanceAndCostAreNotImplemented) {
    EveryDocDISI iterator;
    auto advanced = iterator.advance(3);
    ASSERT_FALSE(advanced.has_value());
    EXPECT_EQ(advanced.error().kind(), ErrorKind::kNotImplemented);
}

TEST(DocIdSetIteratorTest, VariantEnumDispatches) {
    using Enum = DocIdSetIteratorEnum<AllDISI, EmptyDISI>;

    Enum all{std::variant<AllDISI, EmptyDISI>{AllDISI(2)}};
    EXPECT_EQ(all.next_doc().value(), 0);
    EXPECT_EQ(all.next_doc().value(), 1);
    EXPECT_EQ(all.next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(all.cost().value(), 2);

    Enum empty{std::variant<AllDISI, EmptyDISI>{EmptyDISI()}};
    EXPECT_EQ(empty.doc_id(), -1);
    EXPECT_EQ(empty.next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(empty.cost().value(), 0);
}
