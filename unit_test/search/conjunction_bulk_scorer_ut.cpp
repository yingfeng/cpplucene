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

module lucene_core:ut.conjunction_bulk_scorer;

import lucene_core;

using namespace lucene;

namespace {

/// A scorer over a fixed list of documents, with a fixed per-document score and
/// block-based maximum scores.
class WindowScorer final : public Scorer {
public:
    WindowScorer(std::vector<int32_t> docs, float score, int32_t block_size, std::vector<float> block_max)
            : docs_(std::move(docs)),
              score_(score),
              block_size_(block_size),
              block_max_(std::move(block_max)),
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

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_id(); }

    [[nodiscard]] Result<int32_t> advance_shallow(int32_t target) override {
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

    /// The block boundary the last shallow advance was asked for.
    [[nodiscard]] int32_t last_shallow_target() const noexcept { return last_shallow_target_; }

private:
    std::vector<int32_t> docs_;
    float score_;
    int32_t block_size_;
    std::vector<float> block_max_;
    int32_t max_doc_;
    int64_t index_{-1};
    int32_t last_shallow_target_{-1};
};

/// A leaf collector that records the collected documents and their scores.
class RecordingCollector final : public LeafCollector {
public:
    /// When set, the collector raises the minimum competitive score after the
    /// first collected document.
    std::optional<float> raise_after_first;

    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &scorer) override {
        auto score = scorer.score();
        if (!score.has_value()) {
            return std::unexpected(std::move(score.error()));
        }
        docs.push_back(doc);
        scores.push_back(score.value());
        if (raise_after_first.has_value() && docs.size() == 1) {
            return scorer.set_min_competitive_score(raise_after_first.value());
        }
        return {};
    }

    std::vector<int32_t> docs;
    std::vector<float> scores;
};

} // namespace

TEST(ConjunctionBulkScorerTest, RejectsASingleClause) {
    std::vector<std::unique_ptr<Scorer>> scoring;
    scoring.push_back(std::make_unique<WindowScorer>(std::vector<int32_t>{0}, 1.0F, 4, std::vector<float>{1.0F}));
    auto bulk = ConjunctionBulkScorer::create(std::move(scoring), {});
    ASSERT_FALSE(bulk.has_value());
    EXPECT_EQ(bulk.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(ConjunctionBulkScorerTest, CollectsTheIntersection) {
    std::vector<std::unique_ptr<Scorer>> scoring;
    scoring.push_back(std::make_unique<WindowScorer>(std::vector<int32_t>{0, 2, 4, 6}, 2.0F, 4,
                                                     std::vector<float>{10.0F, 10.0F}));
    scoring.push_back(std::make_unique<WindowScorer>(std::vector<int32_t>{2, 4, 6, 8}, 3.0F, 4,
                                                     std::vector<float>{10.0F, 10.0F}));
    std::vector<std::unique_ptr<Scorer>> filters;
    filters.push_back(std::make_unique<WindowScorer>(std::vector<int32_t>{2, 6}, 100.0F, 4,
                                                     std::vector<float>{100.0F, 100.0F}));

    auto bulk = ConjunctionBulkScorer::create(std::move(scoring), std::move(filters));
    ASSERT_TRUE(bulk.has_value()) << bulk.error().to_string();

    RecordingCollector collector;
    auto next = bulk.value()->score(collector, nullptr, 0, 10);
    ASSERT_TRUE(next.has_value()) << next.error().to_string();

    EXPECT_EQ(collector.docs, (std::vector<int32_t>{2, 6}));
    // Only the scoring clauses contribute to the score.
    EXPECT_EQ(collector.scores, (std::vector<float>{5.0F, 5.0F}));
    // The lead clause is the cheapest one, here the two-document filter.
    EXPECT_EQ(bulk.value()->cost().value(), 2);
}

TEST(ConjunctionBulkScorerTest, HonoursTheDocumentWindow) {
    std::vector<std::unique_ptr<Scorer>> scoring;
    scoring.push_back(std::make_unique<WindowScorer>(std::vector<int32_t>{0, 1, 2, 3, 4, 5}, 1.0F, 8,
                                                     std::vector<float>{1.0F}));
    scoring.push_back(std::make_unique<WindowScorer>(std::vector<int32_t>{0, 1, 2, 3, 4, 5}, 1.0F, 8,
                                                     std::vector<float>{1.0F}));

    auto bulk = ConjunctionBulkScorer::create(std::move(scoring), {});
    ASSERT_TRUE(bulk.has_value());

    RecordingCollector collector;
    auto next = bulk.value()->score(collector, nullptr, 2, 4);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{2, 3}));
}

TEST(ConjunctionBulkScorerTest, HonoursTheAcceptedDocuments) {
    std::vector<std::unique_ptr<Scorer>> scoring;
    scoring.push_back(std::make_unique<WindowScorer>(std::vector<int32_t>{0, 1, 2, 3}, 1.0F, 4,
                                                     std::vector<float>{1.0F}));
    scoring.push_back(std::make_unique<WindowScorer>(std::vector<int32_t>{0, 1, 2, 3}, 1.0F, 4,
                                                     std::vector<float>{1.0F}));

    auto bulk = ConjunctionBulkScorer::create(std::move(scoring), {});
    ASSERT_TRUE(bulk.has_value());

    FixedBitSet accept(4);
    accept.set(1);
    accept.set(3);

    RecordingCollector collector;
    auto next = bulk.value()->score(collector, &accept, 0, 4);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{1, 3}));
}

TEST(BlockMaxConjunctionBulkScorerTest, CollectsEveryCompetitiveDocument) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    scorers.push_back(std::make_unique<WindowScorer>(std::vector<int32_t>{0, 1, 2, 3}, 1.0F, 4,
                                                     std::vector<float>{10.0F}));
    scorers.push_back(std::make_unique<WindowScorer>(std::vector<int32_t>{0, 1, 2, 3}, 2.0F, 4,
                                                     std::vector<float>{20.0F}));

    auto bulk = BlockMaxConjunctionBulkScorer::create(4, std::move(scorers));
    ASSERT_TRUE(bulk.has_value()) << bulk.error().to_string();

    RecordingCollector collector;
    auto next = bulk.value()->score(collector, nullptr, 0, 4);
    ASSERT_TRUE(next.has_value()) << next.error().to_string();
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{0, 1, 2, 3}));
    EXPECT_EQ(collector.scores, (std::vector<float>{3.0F, 3.0F, 3.0F, 3.0F}));
    // Equal costs keep the input order, so the first clause leads.
    EXPECT_EQ(bulk.value()->cost().value(), 4);
}

TEST(BlockMaxConjunctionBulkScorerTest, SkipsWindowsBelowTheCompetitiveScore) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    // Block 0 holds documents 0..3 with a maximum score of 10 per clause, block 1
    // holds 4..7 with 50 per clause.
    scorers.push_back(std::make_unique<WindowScorer>(std::vector<int32_t>{0, 1, 2, 3, 4, 5, 6, 7}, 10.0F, 4,
                                                     std::vector<float>{10.0F, 5.0F}));
    scorers.push_back(std::make_unique<WindowScorer>(std::vector<int32_t>{0, 1, 2, 3, 4, 5, 6, 7}, 10.0F, 4,
                                                     std::vector<float>{10.0F, 5.0F}));

    auto bulk = BlockMaxConjunctionBulkScorer::create(8, std::move(scorers));
    ASSERT_TRUE(bulk.has_value());

    RecordingCollector collector;
    // Both clauses together top out at 20 in the first block and at 10 in the
    // second one.
    collector.raise_after_first = 15.0F;
    auto next = bulk.value()->score(collector, nullptr, 0, 8);
    ASSERT_TRUE(next.has_value()) << next.error().to_string();
    // The bar rises after the first document, which is too late for the window it
    // belongs to, but the second window (maximum 10) is skipped entirely.
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{0, 1, 2, 3}));
}
