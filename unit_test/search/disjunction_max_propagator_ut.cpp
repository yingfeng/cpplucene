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

// A `DisjunctionMaxScorer` in `TopScores` mode answers `advance_shallow` from its
// block boundary propagator, so a minimum competitive score can prune whole
// blocks; in the other modes it reports no boundary at all.

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

module lucene_core:ut.disjunction_max_propagator;

import lucene_core;

using namespace lucene;

namespace {

/// A scorer that only serves the block-boundary propagator: a fixed maximum
/// score, a fixed shallow-advance answer and a fixed document.
class BlockScorer final : public Scorer {
public:
    BlockScorer(float max_score, int64_t cost, int32_t doc, int32_t shallow_upto)
            : max_score_(max_score), cost_(cost), doc_(doc), shallow_upto_(shallow_upto) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return doc_; }
    [[nodiscard]] Result<int32_t> advance(int32_t /*target*/) override { return doc_; }
    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }
    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_; }
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override { return max_score_; }
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }
    [[nodiscard]] Result<float> score() override { return max_score_; }

    [[nodiscard]] Result<int32_t> advance_shallow(int32_t target) override {
        last_target_ = target;
        return shallow_upto_;
    }

    [[nodiscard]] int32_t last_target() const noexcept { return last_target_; }

private:
    float max_score_;
    int64_t cost_;
    int32_t doc_;
    int32_t shallow_upto_;
    int32_t last_target_{-1};
};

/// Two block scorers wrapped in a disjunction-max scorer; the scorers stay alive
/// so the test can read what the propagator did with them.
struct Pair {
    std::unique_ptr<DisjunctionMaxScorer> scorer;
    BlockScorer *first{nullptr};
    BlockScorer *second{nullptr};
};

Pair make_pair(ScoreMode score_mode, int32_t first_upto, int32_t second_upto) {
    auto first = std::make_unique<BlockScorer>(10.0F, 100, 0, first_upto);
    auto second = std::make_unique<BlockScorer>(20.0F, 200, 0, second_upto);
    Pair result;
    result.first = first.get();
    result.second = second.get();

    std::vector<std::unique_ptr<Scorer>> sub_scorers;
    sub_scorers.push_back(std::move(first));
    sub_scorers.push_back(std::move(second));
    auto built = DisjunctionMaxScorer::create(0.0F, std::move(sub_scorers), score_mode);
    EXPECT_TRUE(built.has_value()) << (built.has_value() ? "" : built.error().to_string());
    if (built.has_value()) {
        result.scorer = std::move(built.value());
    }
    return result;
}

} // namespace

TEST(DisjunctionMaxPropagatorTest, TopScoresModeAnswersTheBlockBoundary) {
    Pair pair = make_pair(ScoreMode::TopScores, /*first_upto=*/7, /*second_upto=*/5);
    ASSERT_NE(pair.scorer, nullptr);

    // The propagator takes the minimum of the clauses that can still compete, and
    // both clauses were shallow-advanced to the target.
    auto upto = pair.scorer->advance_shallow(3);
    ASSERT_TRUE(upto.has_value()) << upto.error().to_string();
    EXPECT_EQ(upto.value(), 5);
    EXPECT_EQ(pair.first->last_target(), 3);
    EXPECT_EQ(pair.second->last_target(), 3);
}

TEST(DisjunctionMaxPropagatorTest, AnotherScoreModeReportsNoBoundary) {
    for (const ScoreMode mode : {ScoreMode::TopDocs, ScoreMode::Complete, ScoreMode::CompleteNoScores}) {
        Pair pair = make_pair(mode, /*first_upto=*/7, /*second_upto=*/5);
        ASSERT_NE(pair.scorer, nullptr);
        auto upto = pair.scorer->advance_shallow(3);
        ASSERT_TRUE(upto.has_value()) << upto.error().to_string();
        EXPECT_EQ(upto.value(), NO_MORE_DOCS) << static_cast<int>(mode);
        // The clauses were not touched, because there is no propagator.
        EXPECT_EQ(pair.first->last_target(), -1) << static_cast<int>(mode);
        EXPECT_EQ(pair.second->last_target(), -1) << static_cast<int>(mode);
    }
}

TEST(DisjunctionMaxPropagatorTest, RaisingTheThresholdDropsClausesThatCannotCompete) {
    // The first clause scores at most 10, the second at most 20, so a threshold of
    // 15 leaves only the second one to drive the block boundary.
    Pair pair = make_pair(ScoreMode::TopScores, /*first_upto=*/7, /*second_upto=*/5);
    ASSERT_NE(pair.scorer, nullptr);

    auto before = pair.scorer->advance_shallow(0);
    ASSERT_TRUE(before.has_value()) << before.error().to_string();
    EXPECT_EQ(before.value(), 5);

    ASSERT_TRUE(pair.scorer->set_min_competitive_score(15.0F).has_value());
    auto after = pair.scorer->advance_shallow(0);
    ASSERT_TRUE(after.has_value()) << after.error().to_string();
    EXPECT_EQ(after.value(), 5);
    // The clause that can no longer compete is not shallow-advanced any more.
    EXPECT_EQ(pair.first->last_target(), 0);
    EXPECT_EQ(pair.second->last_target(), 0);
}
