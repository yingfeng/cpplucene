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

// The approximate priority queue the flush control uses to pick the per-thread
// buffer with the most RAM: heavier entries are polled first, ties are taken in
// insertion order, and entries without a weight class go to a dense tail.

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>

module lucene_core:ut.approximate_priority_queue;

import lucene_core;

using namespace lucene;

namespace {

/// A queued entry; the queue removes entries by their id.
struct Entry {
    std::string name;

    [[nodiscard]] const std::string &id() const noexcept { return name; }
};

Entry entry(std::string name) { return Entry{std::move(name)}; }

/// Polls every entry and answers their names, in poll order.
std::vector<std::string> drain(ApproximatePriorityQueue<Entry> &queue) {
    std::vector<std::string> names;
    while (true) {
        auto polled = queue.poll([](const Entry &) { return true; });
        if (!polled.has_value()) {
            break;
        }
        names.push_back(polled.value().name);
    }
    return names;
}

} // namespace

TEST(ApproximatePriorityQueueTest, PollsTheHeaviestEntryFirst) {
    ApproximatePriorityQueue<Entry> queue;
    queue.add(entry("light"), int64_t{1} << 3);
    queue.add(entry("heavy"), int64_t{1} << 50);
    queue.add(entry("medium"), int64_t{1} << 20);

    EXPECT_FALSE(queue.is_empty());
    EXPECT_EQ(queue.size(), 3U);
    EXPECT_EQ(drain(queue), (std::vector<std::string>{"heavy", "medium", "light"}));
    EXPECT_TRUE(queue.is_empty());
}

TEST(ApproximatePriorityQueueTest, TiedWeightsComeOutInInsertionOrder) {
    ApproximatePriorityQueue<Entry> queue;
    // The same weight means the same slot class, so the second entry takes the
    // next free slot and comes out after the first one.
    queue.add(entry("first"), int64_t{1} << 20);
    queue.add(entry("second"), int64_t{1} << 20);
    queue.add(entry("third"), int64_t{1} << 20);

    EXPECT_EQ(drain(queue), (std::vector<std::string>{"first", "second", "third"}));
}

TEST(ApproximatePriorityQueueTest, WeightZeroUsesTheDenseTail) {
    ApproximatePriorityQueue<Entry> queue;
    // Weight zero has no leading zeros left, so it does not fit a slot class and
    // goes to the tail, which is drained from its end.
    queue.add(entry("zero-1"), 0);
    queue.add(entry("zero-2"), 0);
    queue.add(entry("zero-3"), 0);

    EXPECT_EQ(drain(queue), (std::vector<std::string>{"zero-3", "zero-2", "zero-1"}));
}

TEST(ApproximatePriorityQueueTest, PollsTheFirstEntryThePredicateAccepts) {
    ApproximatePriorityQueue<Entry> queue;
    queue.add(entry("alpha"), int64_t{1} << 40);
    queue.add(entry("beta"), int64_t{1} << 30);
    queue.add(entry("gamma"), int64_t{1} << 20);

    auto polled = queue.poll([](const Entry &candidate) { return candidate.name.starts_with("b"); });
    ASSERT_TRUE(polled.has_value());
    EXPECT_EQ(polled.value().name, "beta");
    // The other two are still queued.
    EXPECT_EQ(queue.size(), 2U);
    EXPECT_EQ(drain(queue), (std::vector<std::string>{"alpha", "gamma"}));

    EXPECT_FALSE(queue.poll([](const Entry &) { return false; }).has_value());
}

TEST(ApproximatePriorityQueueTest, FindsAndRemovesEntriesById) {
    ApproximatePriorityQueue<Entry> queue;
    queue.add(entry("alpha"), int64_t{1} << 40);
    queue.add(entry("beta"), 0); // the dense tail is searched as well

    EXPECT_TRUE(queue.contains("alpha"));
    EXPECT_TRUE(queue.contains("beta"));
    EXPECT_FALSE(queue.contains("gamma"));

    auto removed = queue.remove("beta");
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(removed.value().name, "beta");
    EXPECT_FALSE(queue.contains("beta"));
    EXPECT_EQ(queue.size(), 1U);

    EXPECT_FALSE(queue.remove("beta").has_value());
    EXPECT_EQ(drain(queue), (std::vector<std::string>{"alpha"}));
    EXPECT_TRUE(queue.is_empty());
}

TEST(ApproximatePriorityQueueTest, HoldsMoreEntriesThanThereAreSlots) {
    ApproximatePriorityQueue<Entry> queue;
    constexpr int32_t kCount = 100;
    for (int32_t i = 0; i < kCount; ++i) {
        queue.add(entry("entry" + std::to_string(i)), int64_t{1} << 40);
    }
    EXPECT_EQ(queue.size(), static_cast<size_t>(kCount));

    const std::vector<std::string> drained = drain(queue);
    ASSERT_EQ(drained.size(), static_cast<size_t>(kCount));
    // Every entry is polled exactly once.
    const std::set<std::string> unique(drained.begin(), drained.end());
    EXPECT_EQ(unique.size(), static_cast<size_t>(kCount));
    EXPECT_TRUE(queue.is_empty());
}

TEST(ApproximatePriorityQueueTest, ANewQueueIsEmpty) {
    ApproximatePriorityQueue<Entry> queue;
    EXPECT_TRUE(queue.is_empty());
    EXPECT_EQ(queue.size(), 0U);
    EXPECT_FALSE(queue.poll([](const Entry &) { return true; }).has_value());
}
