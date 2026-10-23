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
#include <span>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.max_score_bulk_scorer;

import lucene_core;

using namespace lucene;

namespace {

/// A clause over a fixed list of documents with a fixed per-document score and
/// block-based maximum scores.
class ClauseScorer final : public Scorer {
public:
    ClauseScorer(std::vector<int32_t> docs, float score, int32_t block_size, std::vector<float> block_max,
                 int64_t cost)
            : docs_(std::move(docs)),
              score_(score),
              block_size_(block_size),
              block_max_(std::move(block_max)),
              cost_(cost),
              max_doc_(docs_.empty() ? 0 : docs_.back() + 1) {}

    [[nodiscard]] int32_t doc_id() const override {
        if (index_ < 0) {
            return -1;
        }
        return static_cast<size_t>(index_) < docs_.size() ? docs_[static_cast<size_t>(index_)] : NO_MORE_DOCS;
    }

    [[nodiscard]] Result<int32_t> next_doc() override {
        if (index_ < 0) {
            index_ = 0;
            return doc_id();
        }
        const int32_t doc = doc_id();
        if (doc == NO_MORE_DOCS) {
            return NO_MORE_DOCS;
        }
        return advance(doc + 1);
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        if (doc_id() >= target) {
            return doc_id();
        }
        const auto first = index_ < 0 ? docs_.begin() : docs_.begin() + index_;
        index_ = static_cast<int64_t>(std::lower_bound(first, docs_.end(), target) - docs_.begin());
        return doc_id();
    }

    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }
    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_id(); }

    [[nodiscard]] Result<int32_t> advance_shallow(int32_t target) override {
        // Past the last document the clause has nothing left to shallow-advance
        // into, like a real postings enum.
        if (target > max_doc_ - 1) {
            return NO_MORE_DOCS;
        }
        const int32_t block = block_size_ > 0 ? target / block_size_ : 0;
        return std::min((block + 1) * block_size_ - 1, max_doc_ - 1);
    }

    [[nodiscard]] Result<float> get_max_score(int32_t upto) override {
        if (block_max_.empty()) {
            return 0.0F;
        }
        const size_t block = block_size_ > 0 ? static_cast<size_t>(upto / block_size_) : 0U;
        return block_max_[std::min(block, block_max_.size() - 1)];
    }

    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }

    [[nodiscard]] Result<float> score() override { return score_; }

private:
    std::vector<int32_t> docs_;
    float score_;
    int32_t block_size_;
    std::vector<float> block_max_;
    int64_t cost_;
    int32_t max_doc_;
    int64_t index_{-1};
};

/// Records the documents a bulk scorer offers.
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

TEST(MaxScoreBulkScorerTest, RejectsAnEmptyClauseList) {
    auto bulk = MaxScoreBulkScorer::create(10, {}, nullptr);
    ASSERT_FALSE(bulk.has_value());
    EXPECT_EQ(bulk.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(MaxScoreBulkScorerTest, CollectsTheUnionWithSummedScores) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    scorers.push_back(std::make_unique<ClauseScorer>(std::vector<int32_t>{0, 2, 4}, 1.0F, 8,
                                                     std::vector<float>{10.0F}, 3));
    scorers.push_back(std::make_unique<ClauseScorer>(std::vector<int32_t>{0, 1, 3, 4}, 2.0F, 8,
                                                     std::vector<float>{20.0F}, 4));

    auto bulk = MaxScoreBulkScorer::create(5, std::move(scorers), nullptr);
    ASSERT_TRUE(bulk.has_value()) << bulk.error().to_string();
    EXPECT_EQ(bulk.value()->cost().value(), 7);

    RecordingCollector collector;
    auto next = bulk.value()->score(collector, nullptr, 0, 5);
    ASSERT_TRUE(next.has_value()) << next.error().to_string();

    EXPECT_EQ(collector.docs, (std::vector<int32_t>{0, 1, 2, 3, 4}));
    EXPECT_EQ(collector.scores, (std::vector<float>{3.0F, 2.0F, 1.0F, 2.0F, 3.0F}));
    EXPECT_EQ(next.value(), NO_MORE_DOCS);
}

TEST(MaxScoreBulkScorerTest, SkipsWindowsWhenNoClauseCanBeCompetitive) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    scorers.push_back(std::make_unique<ClauseScorer>(std::vector<int32_t>{0, 2, 4}, 1.0F, 8,
                                                     std::vector<float>{10.0F}, 3));
    scorers.push_back(std::make_unique<ClauseScorer>(std::vector<int32_t>{0, 1, 3, 4}, 2.0F, 8,
                                                     std::vector<float>{20.0F}, 4));

    auto bulk = MaxScoreBulkScorer::create(5, std::move(scorers), nullptr);
    ASSERT_TRUE(bulk.has_value());

    // A threshold that no clause can reach makes every clause non-essential, so
    // the whole window is skipped.
    EXPECT_TRUE(bulk.value()->scorable().set_min_competitive_score(1000.0F).has_value());

    RecordingCollector collector;
    auto next = bulk.value()->score(collector, nullptr, 0, 5);
    ASSERT_TRUE(next.has_value()) << next.error().to_string();
    EXPECT_TRUE(collector.docs.empty());
}

TEST(MaxScoreBulkScorerTest, OffersCandidatesFromTheEssentialClausesOnly) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    // The cheap clause can never be competitive on its own, so it becomes
    // non-essential and only adds its score to the documents the other clause
    // already matched.
    scorers.push_back(std::make_unique<ClauseScorer>(std::vector<int32_t>{0, 2, 9}, 1.0F, 16,
                                                     std::vector<float>{1.0F}, 3));
    scorers.push_back(std::make_unique<ClauseScorer>(std::vector<int32_t>{0, 1, 3, 4}, 2.0F, 16,
                                                     std::vector<float>{20.0F}, 4));

    auto bulk = MaxScoreBulkScorer::create(10, std::move(scorers), nullptr);
    ASSERT_TRUE(bulk.has_value());

    EXPECT_TRUE(bulk.value()->scorable().set_min_competitive_score(1.5F).has_value());

    RecordingCollector collector;
    auto next = bulk.value()->score(collector, nullptr, 0, 10);
    ASSERT_TRUE(next.has_value()) << next.error().to_string();

    // Document 2 is only matched by the non-essential clause and its upper bound
    // (1 plus nothing) stays below the threshold, so it is never offered.
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{0, 1, 3, 4}));
    // Only document 0 is also matched by the non-essential clause.
    EXPECT_EQ(collector.scores, (std::vector<float>{3.0F, 2.0F, 2.0F, 2.0F}));
}

TEST(MaxScoreBulkScorerTest, AppliesTheFilter) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    scorers.push_back(std::make_unique<ClauseScorer>(std::vector<int32_t>{0, 1, 2, 3}, 1.0F, 8,
                                                     std::vector<float>{10.0F}, 4));
    scorers.push_back(std::make_unique<ClauseScorer>(std::vector<int32_t>{0, 1, 2, 3}, 1.0F, 8,
                                                     std::vector<float>{10.0F}, 4));
    auto filter = std::make_unique<ClauseScorer>(std::vector<int32_t>{1, 3}, 0.0F, 8, std::vector<float>{0.0F}, 2);

    auto bulk = MaxScoreBulkScorer::create(4, std::move(scorers), std::move(filter));
    ASSERT_TRUE(bulk.has_value());

    RecordingCollector collector;
    auto next = bulk.value()->score(collector, nullptr, 0, 4);
    ASSERT_TRUE(next.has_value()) << next.error().to_string();
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{1, 3}));
    EXPECT_EQ(collector.scores, (std::vector<float>{2.0F, 2.0F}));
}
