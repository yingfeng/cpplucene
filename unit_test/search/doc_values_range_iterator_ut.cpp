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
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.doc_values_range_iterator;

import lucene_core;

using namespace lucene;

namespace {

/// One skipper interval: the documents it covers and the bounds of their values.
struct Interval {
    int32_t min_doc;
    int32_t max_doc;
    int64_t min_value;
    int64_t max_value;
    int32_t doc_count;
};

/// A skipper over a fixed list of level-0 intervals, with the whole range as
/// level 1.
class FakeSkipper final : public DocValuesSkipper {
public:
    explicit FakeSkipper(std::vector<Interval> intervals) : intervals_(std::move(intervals)) {}

    [[nodiscard]] VoidResult advance(int32_t target) override {
        current_ = intervals_.size();
        for (size_t index = 0; index < intervals_.size(); ++index) {
            if (intervals_[index].max_doc >= target) {
                current_ = index;
                return {};
            }
        }
        return {};
    }

    [[nodiscard]] size_t num_levels() const override { return 2; }

    [[nodiscard]] int32_t min_doc_id(size_t level) const override {
        if (level == 1) {
            return intervals_.empty() ? NO_MORE_DOCS : intervals_.front().min_doc;
        }
        return exhausted() ? NO_MORE_DOCS : intervals_[current_].min_doc;
    }

    [[nodiscard]] int32_t max_doc_id(size_t level) const override {
        if (level == 1) {
            return intervals_.empty() ? NO_MORE_DOCS : intervals_.back().max_doc;
        }
        return exhausted() ? NO_MORE_DOCS : intervals_[current_].max_doc;
    }

    [[nodiscard]] int64_t min_value(size_t level) const override {
        if (level == 1) {
            int64_t min = std::numeric_limits<int64_t>::max();
            for (const auto &interval : intervals_) {
                min = std::min(min, interval.min_value);
            }
            return min;
        }
        return exhausted() ? 0 : intervals_[current_].min_value;
    }

    [[nodiscard]] int64_t max_value(size_t level) const override {
        if (level == 1) {
            int64_t max = std::numeric_limits<int64_t>::lowest();
            for (const auto &interval : intervals_) {
                max = std::max(max, interval.max_value);
            }
            return max;
        }
        return exhausted() ? 0 : intervals_[current_].max_value;
    }

    [[nodiscard]] int32_t doc_count(size_t level) const override {
        if (level == 1) {
            int32_t count = 0;
            for (const auto &interval : intervals_) {
                count += interval.doc_count;
            }
            return count;
        }
        return exhausted() ? 0 : intervals_[current_].doc_count;
    }

    [[nodiscard]] int64_t global_min_value() const override { return min_value(1); }
    [[nodiscard]] int64_t global_max_value() const override { return max_value(1); }
    [[nodiscard]] int32_t global_doc_count() const override { return doc_count(1); }


private:
    [[nodiscard]] bool exhausted() const { return current_ >= intervals_.size(); }

    std::vector<Interval> intervals_;
    size_t current_{0};
};

/// A cursor over `0 .. max_doc - 1` that only confirms the documents in
/// `confirmed`, counting how often it was advanced.
class FakeTwoPhase final : public TwoPhaseIterator {
public:
    FakeTwoPhase(int32_t max_doc, std::vector<int32_t> confirmed)
            : max_doc_(max_doc), confirmed_(std::move(confirmed)) {}

    [[nodiscard]] int32_t approx_doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> approx_next_doc() override { return approx_advance(doc_ + 1); }
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override {
        ++advances_;
        doc_ = target < max_doc_ ? target : NO_MORE_DOCS;
        return doc_;
    }
    [[nodiscard]] Result<int64_t> approx_cost() const override { return max_doc_; }
    [[nodiscard]] Result<bool> matches() override {
        return std::ranges::find(confirmed_, doc_) != confirmed_.end();
    }
    [[nodiscard]] float match_cost() const override { return 1.0F; }

    [[nodiscard]] int32_t advances() const noexcept { return advances_; }
    void reset_advances() noexcept { advances_ = 0; }

private:
    int32_t max_doc_;
    std::vector<int32_t> confirmed_;
    int32_t doc_{-1};
    int32_t advances_{0};
};

/// The blocks used by most tests: values grow with the document.
std::vector<Interval> growing_intervals() {
    return {Interval{0, 3, 10, 20, 4}, Interval{4, 7, 30, 40, 4}, Interval{8, 9, 50, 60, 2}};
}

} // namespace

TEST(DocValuesRangeIteratorTest, AcceptsAnIntervalWhoseValuesAllMatch) {
    FakeSkipper skipper(growing_intervals());
    // The inner approximation confirms nothing: an accepted interval must not
    // consult it.
    auto inner = std::make_unique<FakeTwoPhase>(10, std::vector<int32_t>{});
    FakeTwoPhase *inner_observer = inner.get();

    DocValuesRangeIterator iterator(std::move(inner), std::make_shared<FakeSkipper>(growing_intervals()), 30, 40,
                                    false);
    std::vector<int32_t> docs;
    while (true) {
        auto doc = iterator.approx_next_doc();
        ASSERT_TRUE(doc.has_value()) << doc.error().to_string();
        if (doc.value() == NO_MORE_DOCS) {
            break;
        }
        EXPECT_EQ(iterator.match_state(), Match::kYes);
        EXPECT_TRUE(iterator.matches().value());
        docs.push_back(doc.value());
    }
    EXPECT_EQ(docs, (std::vector<int32_t>{4, 5, 6, 7}));
    // The inner approximation was never advanced.
    EXPECT_EQ(inner_observer->advances(), 0);
}

TEST(DocValuesRangeIteratorTest, SkipsIntervalsWhoseValuesAreOutOfRange) {
    auto inner = std::make_unique<FakeTwoPhase>(10, std::vector<int32_t>{});
    FakeTwoPhase *inner_observer = inner.get();
    DocValuesRangeIterator iterator(std::move(inner), std::make_shared<FakeSkipper>(growing_intervals()), 50, 60,
                                    false);

    std::vector<int32_t> docs;
    while (true) {
        auto doc = iterator.approx_next_doc();
        ASSERT_TRUE(doc.has_value());
        if (doc.value() == NO_MORE_DOCS) {
            break;
        }
        EXPECT_TRUE(iterator.matches().value());
        docs.push_back(doc.value());
    }
    // The first two blocks are out of range, so only the last one is visited.
    EXPECT_EQ(docs, (std::vector<int32_t>{8, 9}));
    EXPECT_EQ(inner_observer->advances(), 0);
}

TEST(DocValuesRangeIteratorTest, FallsBackToTheInnerApproximationWhenThereAreGaps) {
    auto inner = std::make_unique<FakeTwoPhase>(10, std::vector<int32_t>{4, 6});
    FakeTwoPhase *inner_observer = inner.get();
    DocValuesRangeIterator iterator(std::move(inner), std::make_shared<FakeSkipper>(growing_intervals()), 30, 40,
                                    true);

    std::vector<int32_t> docs;
    while (true) {
        auto doc = iterator.approx_next_doc();
        ASSERT_TRUE(doc.has_value());
        if (doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto matches = iterator.matches();
        ASSERT_TRUE(matches.has_value());
        if (matches.value()) {
            docs.push_back(doc.value());
        }
    }
    EXPECT_EQ(docs, (std::vector<int32_t>{4, 6}));
    EXPECT_GT(inner_observer->advances(), 0);
}

TEST(DocValuesRangeIteratorTest, ReportsNoMatchWhenEveryValueIsOutOfRange) {
    auto inner = std::make_unique<FakeTwoPhase>(10, std::vector<int32_t>{});
    DocValuesRangeIterator iterator(std::move(inner), std::make_shared<FakeSkipper>(growing_intervals()), 100, 200,
                                    false);

    auto doc = iterator.approx_next_doc();
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc.value(), NO_MORE_DOCS);
    EXPECT_EQ(iterator.approx_cost().value(), 10);
}
