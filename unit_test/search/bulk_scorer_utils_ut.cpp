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

#include <cmath>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

module lucene_core:ut.bulk_scorer_utils;

import lucene_core;

using namespace lucene;

namespace {

/// A scorer that walks `[0, max_doc)` and reports the score `1`, matching only
/// the documents of a set when two-phase iteration is on.
class WalkScorer final : public Scorer {
public:
    WalkScorer(int32_t max_doc, std::set<int32_t> matches) : max_doc_(max_doc), matches_(std::move(matches)) {}

    void set_doc(int32_t doc) noexcept { doc_ = doc; }

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override {
        if (doc_ == NO_MORE_DOCS) {
            return doc_;
        }
        ++doc_;
        if (doc_ >= max_doc_) {
            doc_ = NO_MORE_DOCS;
        }
        return doc_;
    }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_ != NO_MORE_DOCS && doc_ < target) {
            auto next = next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
        }
        return doc_;
    }
    [[nodiscard]] Result<int64_t> cost() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_; }
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override { return 1.0F; }
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::Yes; }
    [[nodiscard]] Result<bool> matches() override { return matches_.contains(doc_); }
    [[nodiscard]] Result<float> score() override { return 1.0F; }

private:
    int32_t max_doc_{0};
    std::set<int32_t> matches_;
    int32_t doc_{-1};
};

/// Scores every document of the requested range.
class WalkBulkScorer final : public BulkScorer {
public:
    explicit WalkBulkScorer(int32_t max_doc) : max_doc_(max_doc) {}

    [[nodiscard]] Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                        int32_t max) override {
        WalkScorer scorer(max_doc_, {});
        if (auto set = collector.set_scorer(scorer); !set.has_value()) {
            return std::unexpected(std::move(set.error()));
        }
        for (int32_t doc = min; doc < max && doc < max_doc_; ++doc) {
            if (accept_docs != nullptr) {
                auto accepted = accept_docs->get(doc);
                if (!accepted.has_value()) {
                    return std::unexpected(std::move(accepted.error()));
                }
                if (!accepted.value()) {
                    continue;
                }
            }
            scorer.set_doc(doc);
            if (auto collected = collector.collect(doc, scorer); !collected.has_value()) {
                return std::unexpected(std::move(collected.error()));
            }
        }
        return max;
    }

    [[nodiscard]] Result<int64_t> cost() override { return max_doc_; }

private:
    int32_t max_doc_{0};
};

/// Records the documents it is handed.
class RecordingLeafCollector final : public LeafCollector {
public:
    [[nodiscard]] VoidResult collect(int32_t doc, Scorable & /*scorer*/) override {
        docs.push_back(doc);
        return {};
    }

    std::vector<int32_t> docs;
};

/// A timeout that fires after a number of checks.
class CountingTimeout final : public QueryTimeout {
public:
    explicit CountingTimeout(int32_t allowed_checks) : allowed_checks_(allowed_checks) {}

    [[nodiscard]] bool should_exit() const override {
        ++checks_;
        return checks_ > allowed_checks_;
    }

private:
    int32_t allowed_checks_{0};
    mutable int32_t checks_{0};
};

} // namespace

TEST(BulkScorerUtilsTest, SimpleScorableKeepsTheScoreAndTheCompetitiveScore) {
    SimpleScorable scorable;
    EXPECT_FLOAT_EQ(scorable.value(), 0.0F);
    ASSERT_TRUE(scorable.set_score(3.0F).has_value());
    EXPECT_FLOAT_EQ(scorable.score().value(), 3.0F);
    ASSERT_TRUE(scorable.set_min_competitive_score(1.5F).has_value());
    EXPECT_FLOAT_EQ(scorable.min_competitive_score(), 1.5F);
}

TEST(BulkScorerUtilsTest, BoostAttributesClearAndClone) {
    BoostAttribute boost;
    EXPECT_FLOAT_EQ(boost.get_boost(), BoostAttribute::kDefaultBoost);
    boost.set_boost(2.0F);
    EXPECT_FLOAT_EQ(boost.get_boost(), 2.0F);
    auto clone = boost.clone();
    ASSERT_NE(clone, nullptr);
    EXPECT_FLOAT_EQ(dynamic_cast<BoostAttribute *>(clone.get())->get_boost(), 2.0F);
    clone->clear();
    EXPECT_FLOAT_EQ(dynamic_cast<BoostAttribute *>(clone.get())->get_boost(), BoostAttribute::kDefaultBoost);
    EXPECT_EQ(boost.name(), "BoostAttribute");

    MaxNonCompetitiveBoostAttribute max_boost;
    EXPECT_TRUE(std::isinf(max_boost.get_max_non_competitive_boost()));
    EXPECT_LT(max_boost.get_max_non_competitive_boost(), 0.0F);
    max_boost.set_max_non_competitive_boost(0.5F);
    max_boost.set_competitive_term(BytesRef::from_string("abc"));
    auto max_clone = max_boost.clone();
    ASSERT_NE(max_clone, nullptr);
    auto *typed = dynamic_cast<MaxNonCompetitiveBoostAttribute *>(max_clone.get());
    ASSERT_NE(typed, nullptr);
    EXPECT_FLOAT_EQ(typed->get_max_non_competitive_boost(), 0.5F);
    ASSERT_TRUE(typed->get_competitive_term().has_value());
    EXPECT_EQ(typed->get_competitive_term().value().utf8_to_string().value(), "abc");
    typed->clear();
    EXPECT_FALSE(typed->get_competitive_term().has_value());
}

TEST(BulkScorerUtilsTest, TimeLimitingBulkScorerScoresWhileTheBudgetLasts) {
    WalkBulkScorer inner(50);
    CountingTimeout timeout(1000);
    TimeLimitingBulkScorer limiting(std::make_unique<WalkBulkScorer>(50), timeout);
    RecordingLeafCollector collector;
    auto scored = limiting.score(collector, nullptr, 0, 50);
    ASSERT_TRUE(scored.has_value()) << scored.error().to_string();
    EXPECT_EQ(scored.value(), 50);
    EXPECT_EQ(collector.docs.size(), 50);
    EXPECT_EQ(limiting.cost().value(), 50);
}

TEST(BulkScorerUtilsTest, TimeLimitingBulkScorerFailsWhenTheBudgetIsGone) {
    CountingTimeout timeout(0);
    TimeLimitingBulkScorer limiting(std::make_unique<WalkBulkScorer>(50), timeout);
    RecordingLeafCollector collector;
    auto scored = limiting.score(collector, nullptr, 0, 50);
    ASSERT_FALSE(scored.has_value());
    EXPECT_EQ(scored.error().kind(), ErrorKind::kTimeExceeded);
    EXPECT_TRUE(collector.docs.empty());
}

TEST(BulkScorerUtilsTest, ReqExclBulkScorerDropsTheExcludedDocuments) {
    WalkBulkScorer required(10);
    auto excluded = std::make_unique<WalkScorer>(10, std::set<int32_t>{2, 5});
    ReqExclBulkScorer scorer(std::make_unique<WalkBulkScorer>(10), std::move(excluded));

    RecordingLeafCollector collector;
    auto upto = scorer.score(collector, nullptr, 0, 10);
    ASSERT_TRUE(upto.has_value()) << upto.error().to_string();
    EXPECT_EQ(upto.value(), 10);
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{0, 1, 3, 4, 6, 7, 8, 9}));
    EXPECT_EQ(scorer.cost().value(), 10);
}

TEST(BulkScorerUtilsTest, ReqExclBulkScorerKeepsEverythingWhenNothingIsExcluded) {
    auto excluded = std::make_unique<WalkScorer>(10, std::set<int32_t>{});
    ReqExclBulkScorer scorer(std::make_unique<WalkBulkScorer>(10), std::move(excluded));

    RecordingLeafCollector collector;
    auto upto = scorer.score(collector, nullptr, 0, 10);
    ASSERT_TRUE(upto.has_value()) << upto.error().to_string();
    EXPECT_EQ(upto.value(), 10);
    EXPECT_EQ(collector.docs.size(), 10);
}
