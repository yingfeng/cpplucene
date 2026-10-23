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
#include <optional>
#include <span>
#include <string>
#include <vector>

module lucene_core:ut.disi_priority_queue;

import lucene_core;

using namespace lucene;

namespace {

using Wrapper = DisiWrapper<std::unique_ptr<Scorer>>;
using Queue = DisiPriorityQueue<std::unique_ptr<Scorer>>;

/// A scorer that only serves the queue: it is positioned through the wrapper.
class IdleScorer final : public Scorer {
public:
    IdleScorer(int32_t doc, int64_t cost) : doc_(doc), cost_(cost) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return ++doc_; }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        doc_ = target;
        return doc_;
    }
    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }
    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_; }
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override { return 1.0F; }
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }
    [[nodiscard]] Result<float> score() override { return 1.0F; }

private:
    int32_t doc_;
    int64_t cost_;
};

/// Builds a wrapper for a scorer that starts on `doc`.
Wrapper make_wrapper(int32_t doc) {
    Wrapper wrapper(std::make_unique<IdleScorer>(doc, 1), 1, 0.0F);
    wrapper.set_doc(doc);
    return wrapper;
}

/// Moves the wrapper at `index` to `doc`, the way a scorer would.
void move_to(std::vector<Wrapper> &wrappers, size_t index, int32_t doc) {
    wrappers[index].set_doc(doc);
    auto advanced = wrappers[index].scorer()->advance(doc);
    EXPECT_TRUE(advanced.has_value());
}

} // namespace

TEST(DisiPriorityQueueTest, OrdersByDocument) {
    Queue queue(3);
    EXPECT_EQ(queue.size(), 0U);
    EXPECT_FALSE(queue.top().has_value());

    std::vector<Wrapper> wrappers;
    wrappers.push_back(make_wrapper(5));
    wrappers.push_back(make_wrapper(2));
    wrappers.push_back(make_wrapper(9));

    // `add` answers the index of the new top.
    EXPECT_EQ(queue.add(0, wrappers), 0U);
    EXPECT_EQ(queue.add(1, wrappers), 1U);
    EXPECT_EQ(queue.add(2, wrappers), 1U);
    EXPECT_EQ(queue.size(), 3U);

    // The top is the wrapper on the least document.
    EXPECT_EQ(queue.top().value(), 1U);
    // And the second least is wrapper 0.
    EXPECT_EQ(queue.top2(wrappers).value(), 0U);
}

TEST(DisiPriorityQueueTest, PopsInDocumentOrder) {
    Queue queue(4);
    std::vector<Wrapper> wrappers;
    const std::vector<int32_t> docs{4, 1, 7, 3};
    for (const int32_t doc : docs) {
        wrappers.push_back(make_wrapper(doc));
    }
    std::vector<size_t> entries{0, 1, 2, 3};
    ASSERT_TRUE(queue.add_all(entries, wrappers).has_value());
    EXPECT_EQ(queue.size(), 4U);

    std::vector<int32_t> popped;
    while (queue.size() > 0) {
        const size_t top = queue.top().value();
        popped.push_back(wrappers[top].doc());
        queue.pop(wrappers);
    }
    EXPECT_EQ(popped, (std::vector<int32_t>{1, 3, 4, 7}));
}

TEST(DisiPriorityQueueTest, UpdateTopAfterTheTopMoved) {
    Queue queue(3);
    std::vector<Wrapper> wrappers;
    wrappers.push_back(make_wrapper(1));
    wrappers.push_back(make_wrapper(2));
    wrappers.push_back(make_wrapper(6));
    for (size_t index = 0; index < wrappers.size(); ++index) {
        queue.add(index, wrappers);
    }
    EXPECT_EQ(queue.top().value(), 0U);

    // The top moved past the second wrapper, so it becomes the top instead.
    move_to(wrappers, 0, 4);
    const size_t top = queue.update_top(wrappers);
    EXPECT_EQ(top, 1U);

    // Replacing the top outright also re-heapifies.
    move_to(wrappers, 1, 10);
    const size_t replaced = queue.update_top_with(1, wrappers);
    EXPECT_EQ(replaced, 0U);
    EXPECT_EQ(wrappers[replaced].doc(), 4);
}

TEST(DisiPriorityQueueTest, TopListLinksTheWrappersOnTheSameDocument) {
    Queue queue(4);
    std::vector<Wrapper> wrappers;
    wrappers.push_back(make_wrapper(3));
    wrappers.push_back(make_wrapper(3));
    wrappers.push_back(make_wrapper(3));
    wrappers.push_back(make_wrapper(8));
    std::vector<size_t> entries{0, 1, 2, 3};
    ASSERT_TRUE(queue.add_all(entries, wrappers).has_value());

    const size_t head = queue.top_list_root(wrappers);
    std::vector<size_t> list;
    for (std::optional<size_t> node = head; node.has_value(); node = wrappers[node.value()].next()) {
        list.push_back(node.value());
    }
    EXPECT_EQ(list.size(), 3U);
    for (const size_t index : list) {
        EXPECT_EQ(wrappers[index].doc(), 3);
    }
}

TEST(DisiPriorityQueueTest, AddAllRejectsOverflow) {
    Queue queue(2);
    std::vector<Wrapper> wrappers;
    wrappers.push_back(make_wrapper(1));
    wrappers.push_back(make_wrapper(2));
    wrappers.push_back(make_wrapper(3));
    std::vector<size_t> entries{0, 1, 2};

    auto added = queue.add_all(entries, wrappers);
    ASSERT_FALSE(added.has_value());
    EXPECT_EQ(added.error().kind(), ErrorKind::kArrayIndexOutOfBounds);
}

TEST(DisiPriorityQueueTest, ClearEmptiesTheQueue) {
    Queue queue(2);
    std::vector<Wrapper> wrappers;
    wrappers.push_back(make_wrapper(1));
    wrappers.push_back(make_wrapper(2));
    queue.add(0, wrappers);
    queue.add(1, wrappers);
    EXPECT_EQ(queue.size(), 2U);

    queue.clear();
    EXPECT_EQ(queue.size(), 0U);
    EXPECT_FALSE(queue.top().has_value());
    EXPECT_EQ(queue.entries().size(), 0U);
}
