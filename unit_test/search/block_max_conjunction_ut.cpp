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

module lucene_core:ut.block_max_conjunction;

import lucene_core;

using namespace lucene;

namespace {

/// A scorer over a fixed list of documents whose score does not depend on the
/// document, with block maximum scores given per block.
class BlockScorer : public Scorer {
public:
    BlockScorer(std::vector<int32_t> docs, float score, int32_t block_size, std::vector<float> block_max)
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
        // Document iterators only move forward, so a target that the cursor has
        // already passed leaves the cursor where it is.
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

    [[nodiscard]] Result<bool> matches() override { return true; }

    [[nodiscard]] Result<float> score() override { return score_; }

protected:
    std::vector<int32_t> docs_;
    float score_;
    int32_t block_size_;
    std::vector<float> block_max_;
    int32_t max_doc_;
    /// `-1` before the first document, like every Lucene document iterator.
    int64_t index_{-1};
};

/// A scorer that only confirms even documents, to exercise two-phase
/// confirmation.
class TwoPhaseBlockScorer final : public BlockScorer {
public:
    using BlockScorer::BlockScorer;

    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::Yes; }

    [[nodiscard]] Result<bool> matches() override { return doc_id() % 2 == 0; }

    [[nodiscard]] float match_cost() const override { return 1.5F; }
};

std::vector<int32_t> drain(DocIdSetIterator &iterator) {
    std::vector<int32_t> docs;
    while (true) {
        auto doc = iterator.next_doc();
        EXPECT_TRUE(doc.has_value()) << (doc.has_value() ? "" : doc.error().to_string());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        docs.push_back(doc.value());
    }
    return docs;
}

} // namespace

TEST(BlockMaxConjunctionTest, PrepareLeadsWithTheCheapestScorer) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    scorers.push_back(std::make_unique<BlockScorer>(std::vector<int32_t>{0, 1, 2, 3, 4}, 1.0F, 4,
                                                    std::vector<float>{1.0F, 1.0F}));
    scorers.push_back(std::make_unique<BlockScorer>(std::vector<int32_t>{0, 1}, 1.0F, 4,
                                                    std::vector<float>{1.0F}));
    scorers.push_back(std::make_unique<BlockScorer>(std::vector<int32_t>{0, 1, 2}, 1.0F, 4,
                                                    std::vector<float>{1.0F}));

    auto scorer = BlockMaxConjunctionScorer::prepare(std::move(scorers));
    ASSERT_TRUE(scorer.has_value()) << scorer.error().to_string();
    ASSERT_EQ(scorer.value()->scorers().size(), 3U);
    EXPECT_EQ(scorer.value()->scorers()[0]->cost().value(), 2);
    EXPECT_EQ(scorer.value()->scorers()[1]->cost().value(), 3);
    EXPECT_EQ(scorer.value()->scorers()[2]->cost().value(), 5);
    EXPECT_EQ(scorer.value()->has_two_phase_iterator(), TwoPhaseState::No);
    EXPECT_FLOAT_EQ(scorer.value()->match_cost(), 0.0F);
}

TEST(BlockMaxConjunctionTest, IteratesTheIntersectionOfTheClauses) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    scorers.push_back(std::make_unique<BlockScorer>(std::vector<int32_t>{0, 2, 4, 6, 8}, 2.0F, 4,
                                                    std::vector<float>{10.0F, 10.0F, 10.0F}));
    scorers.push_back(std::make_unique<BlockScorer>(std::vector<int32_t>{2, 4, 6, 8, 10}, 3.0F, 4,
                                                    std::vector<float>{10.0F, 10.0F, 10.0F}));

    auto scorer = BlockMaxConjunctionScorer::prepare(std::move(scorers));
    ASSERT_TRUE(scorer.has_value()) << scorer.error().to_string();

    // Advancing lands on the next common document.
    auto advanced = scorer.value()->advance(1);
    ASSERT_TRUE(advanced.has_value()) << advanced.error().to_string();
    EXPECT_EQ(advanced.value(), 2);

    auto later = scorer.value()->advance(5);
    ASSERT_TRUE(later.has_value()) << later.error().to_string();
    EXPECT_EQ(later.value(), 6);

    // The score is the sum of the clauses.
    EXPECT_FLOAT_EQ(scorer.value()->score().value(), 5.0F);
    EXPECT_EQ(drain(*scorer.value()), (std::vector<int32_t>{8}));

    auto children = scorer.value()->get_children();
    ASSERT_TRUE(children.has_value());
    EXPECT_EQ(children.value().size(), 2U);
    EXPECT_EQ(children.value()[0].relationship, "MUST");
}

TEST(BlockMaxConjunctionTest, SkipsBlocksBelowTheCompetitiveScore) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    // Documents 0..5, in a cheap block (maximum score 10 per clause) and a
    // competitive one (maximum score 50 per clause).
    scorers.push_back(std::make_unique<BlockScorer>(std::vector<int32_t>{0, 1, 2, 3, 4, 5}, 10.0F, 4,
                                                    std::vector<float>{10.0F, 50.0F}));
    scorers.push_back(std::make_unique<BlockScorer>(std::vector<int32_t>{0, 1, 2, 3, 4, 5}, 10.0F, 4,
                                                    std::vector<float>{10.0F, 50.0F}));

    auto scorer = BlockMaxConjunctionScorer::prepare(std::move(scorers));
    ASSERT_TRUE(scorer.has_value()) << scorer.error().to_string();

    // Both clauses together top out at 20 in the first block, so a threshold of
    // 25 skips documents 0..3.
    EXPECT_TRUE(scorer.value()->set_min_competitive_score(25.0F).has_value());
    EXPECT_EQ(drain(*scorer.value()), (std::vector<int32_t>{4, 5}));
}

TEST(BlockMaxConjunctionTest, ConfirmsTwoPhaseClauses) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    scorers.push_back(std::make_unique<BlockScorer>(std::vector<int32_t>{0, 1, 2, 3}, 1.0F, 4,
                                                    std::vector<float>{10.0F}));
    scorers.push_back(std::make_unique<TwoPhaseBlockScorer>(std::vector<int32_t>{0, 1, 2, 3}, 1.0F, 4,
                                                            std::vector<float>{10.0F}));

    auto scorer = BlockMaxConjunctionScorer::prepare(std::move(scorers));
    ASSERT_TRUE(scorer.has_value()) << scorer.error().to_string();

    EXPECT_EQ(scorer.value()->has_two_phase_iterator(), TwoPhaseState::Yes);
    EXPECT_FLOAT_EQ(scorer.value()->match_cost(), 1.5F);

    auto first = scorer.value()->next_doc();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first.value(), 0);
    EXPECT_TRUE(scorer.value()->matches().value());

    auto second = scorer.value()->next_doc();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value(), 1);
    // The two-phase clause only confirms even documents.
    EXPECT_FALSE(scorer.value()->matches().value());

    auto third = scorer.value()->next_doc();
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(third.value(), 2);
    EXPECT_TRUE(scorer.value()->matches().value());
}

TEST(BlockMaxConjunctionTest, AdvanceShallowUsesTheLeadScorerBoundary) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    scorers.push_back(std::make_unique<BlockScorer>(std::vector<int32_t>{0, 1, 2, 3, 4, 5, 6, 7}, 1.0F, 4,
                                                    std::vector<float>{1.0F, 2.0F}));
    scorers.push_back(std::make_unique<BlockScorer>(std::vector<int32_t>{0, 4, 8}, 1.0F, 8,
                                                    std::vector<float>{3.0F}));

    auto scorer = BlockMaxConjunctionScorer::prepare(std::move(scorers));
    ASSERT_TRUE(scorer.has_value()) << scorer.error().to_string();

    // The cheaper clause leads, and its block boundary wins.
    auto shallow = scorer.value()->advance_shallow(5);
    ASSERT_TRUE(shallow.has_value()) << shallow.error().to_string();
    EXPECT_EQ(shallow.value(), 7);

    // The maximum score is the sum of the block maxima of the clauses.
    auto max_score = scorer.value()->get_max_score(5);
    ASSERT_TRUE(max_score.has_value());
    EXPECT_FLOAT_EQ(max_score.value(), 2.0F + 3.0F);
}

TEST(BlockMaxConjunctionTest, RejectsAnEmptyClauseList) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    auto scorer = BlockMaxConjunctionScorer::prepare(std::move(scorers));
    ASSERT_FALSE(scorer.has_value());
    EXPECT_EQ(scorer.error().kind(), ErrorKind::kIllegalArgument);
}
