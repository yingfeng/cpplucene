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
#include <limits>
#include <memory>
#include <utility>
#include <vector>

module lucene_core:ut.boolean_scorers;

import lucene_core;

using namespace lucene;

namespace {

using Hit = std::pair<int32_t, float>;

/// A scorer over a fixed list of `(doc, score)` pairs; the building block of
/// every test here.
class FixedScorer final : public Scorer {
public:
    explicit FixedScorer(std::vector<Hit> hits) : hits_(std::move(hits)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        if (next_index_ >= hits_.size()) {
            current_ = hits_.size();
            doc_ = NO_MORE_DOCS;
            return doc_;
        }
        current_ = next_index_;
        ++next_index_;
        doc_ = hits_[current_].first;
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        for (;;) {
            auto next = next_doc();
            if (!next.has_value()) {
                return next;
            }
            if (next.value() >= target) {
                return next;
            }
        }
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(hits_.size()); }

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_; }

    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override { return max_score_; }

    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }

    [[nodiscard]] Result<float> score() override {
        if (current_ >= hits_.size()) {
            return 0.0F;
        }
        return hits_[current_].second;
    }

private:
    std::vector<Hit> hits_;
    size_t next_index_{0};
    size_t current_{0};
    int32_t doc_{-1};
    float max_score_{1.0F};
};

/// Collects the document ids a scorer returns, ignoring scores.
Result<std::vector<int32_t>> collect_docs(Scorer &scorer) {
    std::vector<int32_t> docs;
    for (;;) {
        auto doc = scorer.next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        if (doc.value() == NO_MORE_DOCS) {
            return docs;
        }
        docs.push_back(doc.value());
    }
}

/// Collects the documents a two-phase scorer returns: every candidate that
/// confirms.
Result<std::vector<int32_t>> collect_confirmed_docs(Scorer &scorer) {
    std::vector<int32_t> docs;
    for (;;) {
        auto doc = scorer.approx_next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        if (doc.value() == NO_MORE_DOCS) {
            return docs;
        }
        auto matched = scorer.matches();
        if (!matched.has_value()) {
            return std::unexpected(std::move(matched.error()));
        }
        if (matched.value()) {
            docs.push_back(doc.value());
        }
    }
}

/// Builds the sub-scorer list without an `initializer_list` (whose elements are
/// `const` and therefore cannot move a `unique_ptr`).
std::vector<std::unique_ptr<Scorer>> scorers(std::unique_ptr<Scorer> first, std::unique_ptr<Scorer> second) {
    std::vector<std::unique_ptr<Scorer>> result;
    result.push_back(std::move(first));
    result.push_back(std::move(second));
    return result;
}

} // namespace

TEST(ConjunctionDISITest, RequiresAtLeastTwoIterators) {
    std::vector<DocIdSetIterator *> empty;
    auto none = ConjunctionDISI::create(empty);
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().kind(), ErrorKind::kIllegalArgument);

    FixedScorer single({{1, 1.0F}});
    std::vector<DocIdSetIterator *> one{&single};
    auto too_few = ConjunctionDISI::create(one);
    ASSERT_FALSE(too_few.has_value());
    EXPECT_EQ(too_few.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(ConjunctionDISITest, RejectsIteratorsOnDifferentDocuments) {
    FixedScorer left({{1, 1.0F}});
    FixedScorer right({{2, 1.0F}});
    ASSERT_TRUE(left.next_doc().has_value());
    ASSERT_TRUE(right.next_doc().has_value());

    std::vector<DocIdSetIterator *> iterators{&left, &right};
    auto conjunction = ConjunctionDISI::create(iterators);
    ASSERT_FALSE(conjunction.has_value());
    EXPECT_EQ(conjunction.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(ConjunctionUtilsTest, UnwrapsZeroAndOneIterator) {
    auto none = ConjunctionUtils::intersect_iterators({});
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none.value(), nullptr);

    auto single = std::make_unique<FixedScorer>(std::vector<Hit>{{1, 1.0F}});
    const FixedScorer *borrowed = single.get();
    std::vector<std::unique_ptr<DocIdSetIterator>> iterators;
    iterators.push_back(std::move(single));
    auto one = ConjunctionUtils::intersect_iterators(std::move(iterators));
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one.value().get(), borrowed);
}

TEST(ConjunctionScorerTest, IntersectsDocumentsAndSumsScores) {
    auto first = std::make_unique<FixedScorer>(std::vector<Hit>{{1, 2.0F}, {2, 3.0F}, {4, 5.0F}});
    auto second = std::make_unique<FixedScorer>(std::vector<Hit>{{2, 7.0F}, {3, 11.0F}, {4, 13.0F}});

    auto scoring = ConjunctionUtils::intersect_scorers(scorers(std::move(first), std::move(second)), {0, 1});
    ASSERT_TRUE(scoring.has_value());
    ASSERT_NE(scoring.value(), nullptr);

    auto docs = collect_docs(*scoring.value());
    ASSERT_TRUE(docs.has_value());
    EXPECT_EQ(docs.value(), (std::vector<int32_t>{2, 4}));

    // Re-run to check the scores: the sum of both clauses.
    auto score_first = std::make_unique<FixedScorer>(std::vector<Hit>{{2, 3.0F}});
    auto score_second = std::make_unique<FixedScorer>(std::vector<Hit>{{2, 7.0F}});
    auto summed = ConjunctionUtils::intersect_scorers(scorers(std::move(score_first), std::move(score_second)), {0, 1});
    ASSERT_TRUE(summed.has_value());
    ASSERT_TRUE(summed.value()->next_doc().has_value());
    EXPECT_EQ(summed.value()->doc_id(), 2);
    EXPECT_EQ(summed.value()->score().value(), 10.0F);
    EXPECT_EQ(summed.value()->get_max_score(NO_MORE_DOCS).value(), 2.0F);
}

TEST(ConjunctionScorerTest, OnlyScoringClausesContributeToTheScore) {
    // Clause 0 is a filter: it intersects but does not score.
    auto filter = std::make_unique<FixedScorer>(std::vector<Hit>{{5, 100.0F}});
    auto scored = std::make_unique<FixedScorer>(std::vector<Hit>{{5, 3.0F}});

    auto conjunction = ConjunctionScorer::create(scorers(std::move(filter), std::move(scored)), {1});
    ASSERT_TRUE(conjunction.has_value());
    ASSERT_TRUE(conjunction.value()->next_doc().has_value());
    EXPECT_EQ(conjunction.value()->doc_id(), 5);
    EXPECT_EQ(conjunction.value()->score().value(), 3.0F);
}

TEST(DisjunctionScorerTest, UnionsDocumentsAndSumsMatchingScores) {
    auto first = std::make_unique<FixedScorer>(std::vector<Hit>{{1, 2.0F}, {3, 4.0F}});
    auto second = std::make_unique<FixedScorer>(std::vector<Hit>{{2, 8.0F}, {3, 16.0F}});

    auto disjunction = DisjunctionSumScorer::create(scorers(std::move(first), std::move(second)));
    ASSERT_TRUE(disjunction.has_value());

    auto docs = collect_docs(*disjunction.value());
    ASSERT_TRUE(docs.has_value());
    EXPECT_EQ(docs.value(), (std::vector<int32_t>{1, 2, 3}));

    // Only document 3 matches both clauses.
    auto score_first = std::make_unique<FixedScorer>(std::vector<Hit>{{3, 4.0F}});
    auto score_second = std::make_unique<FixedScorer>(std::vector<Hit>{{3, 16.0F}});
    auto summed = DisjunctionSumScorer::create(scorers(std::move(score_first), std::move(score_second)));
    ASSERT_TRUE(summed.has_value());
    ASSERT_TRUE(summed.value()->next_doc().has_value());
    EXPECT_EQ(summed.value()->doc_id(), 3);
    EXPECT_EQ(summed.value()->score().value(), 20.0F);
    EXPECT_EQ(summed.value()->freq().value(), 2);
}

TEST(DisjunctionScorerTest, AdvancingSkipsDocumentsBelowTheTarget) {
    auto first = std::make_unique<FixedScorer>(std::vector<Hit>{{1, 1.0F}, {10, 1.0F}});
    auto second = std::make_unique<FixedScorer>(std::vector<Hit>{{2, 1.0F}, {20, 1.0F}});
    auto disjunction = DisjunctionSumScorer::create(scorers(std::move(first), std::move(second)));
    ASSERT_TRUE(disjunction.has_value());

    EXPECT_EQ(disjunction.value()->advance(5).value(), 10);
    EXPECT_EQ(disjunction.value()->next_doc().value(), 20);
    EXPECT_EQ(disjunction.value()->next_doc().value(), NO_MORE_DOCS);
}

TEST(ReqOptSumScorerTest, KeepsTheRequiredDocumentsAndAddsTheOptionalScore) {
    auto required = std::make_unique<FixedScorer>(std::vector<Hit>{{1, 5.0F}, {3, 7.0F}});
    auto optional = std::make_unique<FixedScorer>(std::vector<Hit>{{2, 11.0F}, {3, 13.0F}});

    auto scorer = ReqOptSumScorer::create(std::move(required), std::move(optional));
    ASSERT_TRUE(scorer.has_value());

    auto docs = collect_docs(*scorer.value());
    ASSERT_TRUE(docs.has_value());
    EXPECT_EQ(docs.value(), (std::vector<int32_t>{1, 3}));

    // Document 3 is matched by both clauses, so its score is the sum.
    auto required_again = std::make_unique<FixedScorer>(std::vector<Hit>{{1, 5.0F}, {3, 7.0F}});
    auto optional_again = std::make_unique<FixedScorer>(std::vector<Hit>{{2, 11.0F}, {3, 13.0F}});
    auto summed = ReqOptSumScorer::create(std::move(required_again), std::move(optional_again));
    ASSERT_TRUE(summed.has_value());

    ASSERT_TRUE(summed.value()->next_doc().has_value());
    EXPECT_EQ(summed.value()->doc_id(), 1);
    EXPECT_EQ(summed.value()->score().value(), 5.0F);

    ASSERT_TRUE(summed.value()->next_doc().has_value());
    EXPECT_EQ(summed.value()->doc_id(), 3);
    EXPECT_EQ(summed.value()->score().value(), 20.0F);
}

TEST(ReqOptSumScorerTest, RejectsMissingChildren) {
    auto only_required = ReqOptSumScorer::create(std::make_unique<FixedScorer>(std::vector<Hit>{{1, 1.0F}}), nullptr);
    ASSERT_FALSE(only_required.has_value());
    EXPECT_EQ(only_required.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(ReqExclScorerTest, RejectsExcludedDocuments) {
    auto required = std::make_unique<FixedScorer>(std::vector<Hit>{{1, 1.0F}, {2, 1.0F}, {3, 1.0F}, {4, 1.0F}});
    auto excluded = std::make_unique<FixedScorer>(std::vector<Hit>{{2, 1.0F}, {4, 1.0F}});

    auto scorer = ReqExclScorer::create(std::move(required), std::move(excluded));
    ASSERT_TRUE(scorer.has_value());
    EXPECT_EQ(scorer.value()->has_two_phase_iterator(), TwoPhaseState::Yes);

    auto docs = collect_confirmed_docs(*scorer.value());
    ASSERT_TRUE(docs.has_value());
    EXPECT_EQ(docs.value(), (std::vector<int32_t>{1, 3}));
}

TEST(ReqExclScorerTest, AdvancesTheExcludedClauseAlongWithTheRequiredOne) {
    auto required = std::make_unique<FixedScorer>(std::vector<Hit>{{10, 2.0F}, {20, 3.0F}, {30, 4.0F}});
    auto excluded = std::make_unique<FixedScorer>(std::vector<Hit>{{20, 1.0F}});

    auto scorer = ReqExclScorer::create(std::move(required), std::move(excluded));
    ASSERT_TRUE(scorer.has_value());

    // Every candidate comes from the required clause; only 20 is rejected.
    EXPECT_EQ(scorer.value()->approx_next_doc().value(), 10);
    EXPECT_TRUE(scorer.value()->matches().value());
    EXPECT_EQ(scorer.value()->score().value(), 2.0F);

    EXPECT_EQ(scorer.value()->approx_advance(15).value(), 20);
    EXPECT_FALSE(scorer.value()->matches().value());

    EXPECT_EQ(scorer.value()->approx_next_doc().value(), 30);
    EXPECT_TRUE(scorer.value()->matches().value());
    EXPECT_EQ(scorer.value()->score().value(), 4.0F);

    EXPECT_EQ(scorer.value()->approx_next_doc().value(), NO_MORE_DOCS);
}
