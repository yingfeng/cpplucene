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
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.score_caching;

import lucene_core;

using namespace lucene;

namespace {

/// A [`Scorable`] that counts how often it was asked to score.
class CountingScorable final : public Scorable {
public:
    explicit CountingScorable(float score) : score_(score) {}

    [[nodiscard]] Result<float> score() override {
        ++calls_;
        return score_;
    }

    [[nodiscard]] int32_t calls() const noexcept { return calls_; }

private:
    float score_;
    int32_t calls_{0};
};

/// A leaf collector that asks for the score twice per document, which is what
/// makes the cache observable.
class TwiceScoringCollector final : public LeafCollector {
public:
    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &scorer) override {
        auto first = scorer.score();
        if (!first.has_value()) {
            return std::unexpected(std::move(first.error()));
        }
        auto second = scorer.score();
        if (!second.has_value()) {
            return std::unexpected(std::move(second.error()));
        }
        docs_.push_back(doc);
        scores_.push_back(first.value());
        // Both reads of one document must agree.
        if (first.value() != second.value()) {
            return err(LuceneError::illegal_state("the score changed between two reads"));
        }
        return {};
    }

    [[nodiscard]] const std::vector<int32_t> &docs() const noexcept { return docs_; }
    [[nodiscard]] const std::vector<float> &scores() const noexcept { return scores_; }

private:
    std::vector<int32_t> docs_;
    std::vector<float> scores_;
};

} // namespace

TEST(ScoreCachingTest, WrapperComputesTheScoreOnce) {
    CountingScorable inner(2.5F);
    CachedScore cache;
    ScoreCachingWrappingScorer wrapper(inner, cache);

    EXPECT_FLOAT_EQ(wrapper.score().value(), 2.5F);
    EXPECT_FLOAT_EQ(wrapper.score().value(), 2.5F);
    EXPECT_EQ(inner.calls(), 1);
    EXPECT_TRUE(cache.is_cached());

    cache.invalidate();
    EXPECT_FALSE(cache.is_cached());
    EXPECT_FLOAT_EQ(wrapper.score().value(), 2.5F);
    EXPECT_EQ(inner.calls(), 2);
}

TEST(ScoreCachingTest, WrapperForwardsTheCompetitiveScore) {
    // `set_min_competitive_score` goes through to the wrapped scorable, which the
    // default implementation accepts.
    CountingScorable inner(1.0F);
    CachedScore cache;
    ScoreCachingWrappingScorer wrapper(inner, cache);
    EXPECT_TRUE(wrapper.set_min_competitive_score(3.0F).has_value());
    EXPECT_TRUE(wrapper.get_children().has_value());
}

TEST(ScoreCachingTest, LeafCollectorScoresEachDocumentOnce) {
    auto inner = std::make_unique<TwiceScoringCollector>();
    TwiceScoringCollector *observed = inner.get();
    ScoreCachingLeafCollector collector(std::move(inner));

    CountingScorable scorer(1.5F);
    ASSERT_TRUE(collector.set_scorer(scorer).has_value());
    ASSERT_TRUE(collector.collect(0, scorer).has_value());
    ASSERT_TRUE(collector.collect(1, scorer).has_value());
    ASSERT_TRUE(collector.collect(2, scorer).has_value());

    // Two reads per document, but only one computation.
    EXPECT_EQ(scorer.calls(), 3);
    EXPECT_EQ(observed->docs(), (std::vector<int32_t>{0, 1, 2}));
    EXPECT_EQ(observed->scores(), (std::vector<float>{1.5F, 1.5F, 1.5F}));
    EXPECT_TRUE(collector.finish().has_value());
} 
