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

module lucene_core:ut.long_heap;

import lucene_core;

using namespace lucene;

TEST(LongHeapTest, RejectsInvalidSize) {
    EXPECT_FALSE(LongHeap::create(0).has_value());
    EXPECT_TRUE(LongHeap::create(1).has_value());
}

TEST(LongHeapTest, PushPopIsAscending) {
    auto heap = LongHeap::create(8);
    ASSERT_TRUE(heap.has_value());

    for (int64_t value : {5, 3, 9, 1, 7}) {
        ASSERT_TRUE(heap->push(value).has_value());
    }
    EXPECT_EQ(heap->size(), 5u);
    EXPECT_EQ(heap->top(), 1); // least element

    std::vector<int64_t> popped;
    while (heap->size() > 0) {
        auto value = heap->pop();
        ASSERT_TRUE(value.has_value());
        popped.push_back(value.value());
    }
    EXPECT_EQ(popped, (std::vector<int64_t>{1, 3, 5, 7, 9}));

    // Popping an empty heap is an error.
    EXPECT_FALSE(heap->pop().has_value());
}

TEST(LongHeapTest, InsertWithOverflowKeepsLargestValues) {
    auto heap = LongHeap::create(3);
    ASSERT_TRUE(heap.has_value());

    EXPECT_TRUE(heap->insert_with_overflow(10).value());
    EXPECT_TRUE(heap->insert_with_overflow(5).value());
    EXPECT_TRUE(heap->insert_with_overflow(7).value());
    // Full: a smaller value is dropped ...
    EXPECT_FALSE(heap->insert_with_overflow(1).value());
    // ... and a larger value replaces the current minimum.
    EXPECT_TRUE(heap->insert_with_overflow(42).value());

    int64_t a = heap->get(1);
    int64_t b = heap->get(2);
    int64_t c = heap->get(3);
    if (a > b) {
        const int64_t tmp = a;
        a = b;
        b = tmp;
    }
    if (b > c) {
        const int64_t tmp = b;
        b = c;
        c = tmp;
    }
    if (a > b) {
        const int64_t tmp = a;
        a = b;
        b = tmp;
    }
    EXPECT_EQ(a, 7);
    EXPECT_EQ(b, 10);
    EXPECT_EQ(c, 42);
}

TEST(LongHeapTest, UpdateTopReplacesMinimum) {
    auto heap = LongHeap::create(4);
    ASSERT_TRUE(heap.has_value());
    for (int64_t value : {4, 2, 6}) {
        ASSERT_TRUE(heap->push(value).has_value());
    }

    EXPECT_EQ(heap->update_top(3), 3);
    EXPECT_EQ(heap->update_top(9), 4);
    EXPECT_EQ(heap->size(), 3u);
}

TEST(LongHeapTest, PushAllAndClear) {
    auto first = LongHeap::create(4);
    auto second = LongHeap::create(4);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    ASSERT_TRUE(first->push(8).has_value());
    ASSERT_TRUE(first->push(2).has_value());
    ASSERT_TRUE(second->push(6).has_value());

    ASSERT_TRUE(second->push_all(*first).has_value());
    EXPECT_EQ(second->size(), 3u);
    EXPECT_EQ(second->top(), 2);

    second->clear();
    EXPECT_EQ(second->size(), 0u);
}

TEST(LongHeapTest, GrowsBeyondInitialCapacity) {
    auto heap = LongHeap::create(1);
    ASSERT_TRUE(heap.has_value());
    for (int64_t value = 64; value > 0; --value) {
        ASSERT_TRUE(heap->push(value).has_value());
    }
    EXPECT_EQ(heap->size(), 64u);
    EXPECT_EQ(heap->top(), 1);
}
