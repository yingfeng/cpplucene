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
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.disjunction_max_bulk_scorer;

import lucene_core;

using namespace lucene;

namespace {

/// A sub-scorer that collects a fixed list of documents, each with a fixed
/// score.
class FixedBulkScorer final : public BulkScorer {
public:
    FixedBulkScorer(std::vector<int32_t> docs, float score, int64_t cost)
            : docs_(std::move(docs)), score_(score), cost_(cost) {}

    [[nodiscard]] Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                        int32_t max) override {
        for (const int32_t doc : docs_) {
            if (doc < min || doc >= max) {
                continue;
            }
            if (accept_docs != nullptr) {
                auto accepted = accept_docs->get(static_cast<size_t>(doc));
                if (!accepted.has_value()) {
                    return std::unexpected(std::move(accepted.error()));
                }
                if (!accepted.value()) {
                    continue;
                }
            }
            auto collected = collector.collect(doc, scorable_);
            if (!collected.has_value()) {
                return std::unexpected(std::move(collected.error()));
            }
        }
        // The first document at or after `max`.
        const auto next = std::lower_bound(docs_.begin(), docs_.end(), max);
        return next == docs_.end() ? NO_MORE_DOCS : *next;
    }

    [[nodiscard]] Result<int64_t> cost() override { return cost_; }

private:
    /// The score handed to the collector.
    class FixedScorable final : public Scorable {
    public:
        explicit FixedScorable(float score) : score_(score) {}
        [[nodiscard]] Result<float> score() override { return score_; }

    private:
        float score_;
    };

    std::vector<int32_t> docs_;
    float score_;
    int64_t cost_;
    FixedScorable scorable_{score_};
};

/// Records what a bulk scorer collects.
class RecordingCollector final : public LeafCollector {
public:
    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &scorer) override {
        auto score = scorer.score();
        if (!score.has_value()) {
            return std::unexpected(std::move(score.error()));
        }
        docs.push_back(doc);
        scores.push_back(score.value());
        return {};
    }

    std::vector<int32_t> docs;
    std::vector<float> scores;
};

} // namespace

TEST(DisjunctionMaxBulkScorerTest, RejectsASingleSubScorer) {
    std::vector<std::unique_ptr<BulkScorer>> sub;
    sub.push_back(std::make_unique<FixedBulkScorer>(std::vector<int32_t>{0}, 1.0F, 1));
    auto bulk = DisjunctionMaxBulkScorer::create(std::move(sub));
    ASSERT_FALSE(bulk.has_value());
    EXPECT_EQ(bulk.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(DisjunctionMaxBulkScorerTest, CollectsTheUnionWithTheBestScore) {
    std::vector<std::unique_ptr<BulkScorer>> sub;
    sub.push_back(std::make_unique<FixedBulkScorer>(std::vector<int32_t>{1, 2, 3}, 1.0F, 3));
    sub.push_back(std::make_unique<FixedBulkScorer>(std::vector<int32_t>{2, 3, 4}, 5.0F, 3));

    auto bulk = DisjunctionMaxBulkScorer::create(std::move(sub));
    ASSERT_TRUE(bulk.has_value()) << bulk.error().to_string();

    RecordingCollector collector;
    auto next = bulk.value()->score(collector, nullptr, 0, 10);
    ASSERT_TRUE(next.has_value()) << next.error().to_string();

    // Every document is collected once, in order, with the maximum sub-score.
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{1, 2, 3, 4}));
    EXPECT_EQ(collector.scores, (std::vector<float>{1.0F, 5.0F, 5.0F, 5.0F}));
    EXPECT_EQ(bulk.value()->cost().value(), 6);
}

TEST(DisjunctionMaxBulkScorerTest, HonoursTheDocumentWindow) {
    std::vector<std::unique_ptr<BulkScorer>> sub;
    sub.push_back(std::make_unique<FixedBulkScorer>(std::vector<int32_t>{0, 1, 2, 3, 4}, 2.0F, 1));
    sub.push_back(std::make_unique<FixedBulkScorer>(std::vector<int32_t>{0, 1, 2, 3, 4}, 3.0F, 1));

    auto bulk = DisjunctionMaxBulkScorer::create(std::move(sub));
    ASSERT_TRUE(bulk.has_value());

    RecordingCollector collector;
    auto next = bulk.value()->score(collector, nullptr, 2, 4);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{2, 3}));
    EXPECT_EQ(collector.scores, (std::vector<float>{3.0F, 3.0F}));
}

TEST(DisjunctionMaxBulkScorerTest, SplitsLongRangesIntoWindows) {
    // The documents sit in two windows, one at the start of the range and one
    // beyond the window size.
    std::vector<int32_t> first{0, 1};
    std::vector<int32_t> second{static_cast<int32_t>(kDisjunctionMaxWindowSize) + 5};
    std::vector<std::unique_ptr<BulkScorer>> sub;
    sub.push_back(std::make_unique<FixedBulkScorer>(first, 1.0F, 1));
    sub.push_back(std::make_unique<FixedBulkScorer>(second, 2.0F, 1));

    auto bulk = DisjunctionMaxBulkScorer::create(std::move(sub));
    ASSERT_TRUE(bulk.has_value());

    RecordingCollector collector;
    auto next = bulk.value()->score(collector, nullptr, 0,
                                    static_cast<int32_t>(kDisjunctionMaxWindowSize) + 10);
    ASSERT_TRUE(next.has_value()) << next.error().to_string();
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{0, 1, static_cast<int32_t>(kDisjunctionMaxWindowSize) + 5}));
    EXPECT_EQ(collector.scores, (std::vector<float>{1.0F, 1.0F, 2.0F}));
}
