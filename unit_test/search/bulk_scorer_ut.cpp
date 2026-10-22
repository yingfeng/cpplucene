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
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.bulk_scorer;

import lucene_core;

using namespace lucene;

namespace {

/// A scorer over a fixed list of documents; the score of a document is its id
/// as a float, so the collector can check it.
class ListScorer final : public Scorer {
public:
    ListScorer(std::vector<int32_t> docs, std::vector<bool> accepted)
        : docs_(std::move(docs)), accepted_(std::move(accepted)) {}

    [[nodiscard]] int32_t doc_id() const override { return index_ < 0 ? -1 : (index_ < size() ? docs_[index_] : NO_MORE_DOCS); }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++index_;
        return doc_id();
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        if (index_ >= 0 && doc_id() >= target) {
            return doc_id();
        }
        do {
            ++index_;
        } while (doc_id() < target && doc_id() != NO_MORE_DOCS);
        return doc_id();
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(size()); }

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_id(); }

    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override {
        return static_cast<float>(docs_.empty() ? 0 : docs_.back());
    }

    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override {
        return accepted_.empty() ? TwoPhaseState::No : TwoPhaseState::Yes;
    }

    [[nodiscard]] Result<float> score() override { return static_cast<float>(docs_[static_cast<size_t>(index_)]); }

    /// Only "even" documents are accepted when a filter was supplied.
    [[nodiscard]] Result<bool> matches() override {
        return accepted_.empty() || accepted_[static_cast<size_t>(index_)];
    }

private:
    [[nodiscard]] size_t size() const noexcept { return docs_.size(); }

    std::vector<int32_t> docs_;
    std::vector<bool> accepted_;
    int32_t index_{-1};
};

/// Records the documents it is given, and their scores when it asks for them.
class RecordingCollector final : public LeafCollector {
public:
    std::vector<int32_t> docs;
    std::vector<float> scores;
    Scorable *scorer{nullptr};
    int32_t finish_calls{0};

    [[nodiscard]] VoidResult set_scorer(Scorable &value) override {
        scorer = &value;
        return {};
    }

    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &value) override {
        docs.push_back(doc);
        auto score = value.score();
        EXPECT_TRUE(score.has_value());
        if (score.has_value()) {
            scores.push_back(score.value());
        }
        return {};
    }

    [[nodiscard]] VoidResult finish() override {
        ++finish_calls;
        return {};
    }
};

} // namespace

TEST(BulkScorerTest, ScoresEveryDocument) {
    auto scorer = std::make_unique<ListScorer>(std::vector<int32_t>{0, 2, 5, 9}, std::vector<bool>{});
    DefaultBulkScorer bulk(std::move(scorer));
    RecordingCollector collector;

    auto next = bulk.score(collector, nullptr, 0, NO_MORE_DOCS);
    ASSERT_TRUE(next.has_value()) << (next ? "" : next.error().to_string());
    EXPECT_EQ(next.value(), NO_MORE_DOCS);
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{0, 2, 5, 9}));
    EXPECT_EQ(collector.scores, (std::vector<float>{0.0F, 2.0F, 5.0F, 9.0F}));
    EXPECT_TRUE(collector.scorer != nullptr) << "set_scorer must be called before collecting";

    auto cost = bulk.cost();
    ASSERT_TRUE(cost.has_value());
    EXPECT_EQ(cost.value(), 4);
}

TEST(BulkScorerTest, HonoursAcceptDocs) {
    auto scorer = std::make_unique<ListScorer>(std::vector<int32_t>{0, 2, 5, 9}, std::vector<bool>{});
    DefaultBulkScorer bulk(std::move(scorer));
    RecordingCollector collector;

    FixedBitSet accept(16);
    accept.set(0);
    accept.set(5);
    accept.set(9);

    auto next = bulk.score(collector, &accept, 0, NO_MORE_DOCS);
    ASSERT_TRUE(next.has_value()) << (next ? "" : next.error().to_string());
    // A filter forces the ranged path, which reports where it stopped.
    EXPECT_EQ(next.value(), NO_MORE_DOCS);
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{0, 5, 9}));
}

TEST(BulkScorerTest, HonoursTheRequestedRange) {
    auto scorer = std::make_unique<ListScorer>(std::vector<int32_t>{0, 2, 5, 9}, std::vector<bool>{});
    DefaultBulkScorer bulk(std::move(scorer));
    RecordingCollector collector;

    auto next = bulk.score(collector, nullptr, 3, 9);
    ASSERT_TRUE(next.has_value()) << (next ? "" : next.error().to_string());
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{5}));

    // The same range again on a fresh scorer, with a max beyond the last doc.
    auto other = std::make_unique<ListScorer>(std::vector<int32_t>{0, 2, 5, 9}, std::vector<bool>{});
    DefaultBulkScorer other_bulk(std::move(other));
    RecordingCollector other_collector;
    auto next_other = other_bulk.score(other_collector, nullptr, 2, 100);
    ASSERT_TRUE(next_other.has_value());
    EXPECT_EQ(other_collector.docs, (std::vector<int32_t>{2, 5, 9}));
}

TEST(BulkScorerTest, ConfirmsCandidatesOfATwoPhaseScorer) {
    // Only even positions are accepted, so the odd candidates must be filtered.
    auto scorer = std::make_unique<ListScorer>(std::vector<int32_t>{1, 2, 3, 4}, std::vector<bool>{false, true, false, true});
    DefaultBulkScorer bulk(std::move(scorer));
    RecordingCollector collector;

    auto next = bulk.score(collector, nullptr, 0, NO_MORE_DOCS);
    ASSERT_TRUE(next.has_value()) << (next ? "" : next.error().to_string());
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{2, 4}));
}

TEST(BulkScorerTest, StopsAtTheEndOfTheRange) {
    auto scorer = std::make_unique<ListScorer>(std::vector<int32_t>{0, 1, 2, 3, 4}, std::vector<bool>{});
    DefaultBulkScorer bulk(std::move(scorer));
    RecordingCollector collector;

    // `max` is exclusive.
    auto next = bulk.score(collector, nullptr, 1, 3);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{1, 2}));
}

TEST(TwoPhaseIteratorTest, SkipsRejectedCandidates) {
    /// Accepts only even candidates.
    class EvenTPI final : public TwoPhaseIterator {
    public:
        [[nodiscard]] int32_t approx_doc_id() const override { return doc_; }
        [[nodiscard]] Result<int32_t> approx_next_doc() override {
            doc_ = doc_ == -1 ? 0 : doc_ + 1;
            if (doc_ > 6) {
                doc_ = NO_MORE_DOCS;
            }
            return doc_;
        }
        [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override {
            while (doc_ < target) {
                if (auto next = approx_next_doc(); !next.has_value()) {
                    return next;
                }
            }
            return doc_;
        }
        [[nodiscard]] Result<int64_t> approx_cost() const override { return int64_t{7}; }
        [[nodiscard]] Result<bool> matches() override { return doc_ % 2 == 0; }
        [[nodiscard]] float match_cost() const override { return 1.5F; }

    private:
        int32_t doc_{-1};
    };

    auto iterator = as_doc_id_set_iterator(std::make_unique<EvenTPI>());
    ASSERT_TRUE(iterator != nullptr);
    EXPECT_EQ(iterator->doc_id(), -1);
    EXPECT_EQ(iterator->next_doc().value(), 0);
    EXPECT_EQ(iterator->next_doc().value(), 2);
    EXPECT_EQ(iterator->advance(3).value(), 4);
    EXPECT_EQ(iterator->advance(6).value(), 6);
    EXPECT_EQ(iterator->next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(iterator->cost().value(), 7);

    // `approx_slow_advance` is a linear fallback over the candidates.
    class SlowTPI final : public TwoPhaseIterator {
    public:
        [[nodiscard]] int32_t approx_doc_id() const override { return doc_; }
        [[nodiscard]] Result<int32_t> approx_next_doc() override {
            doc_ = doc_ == -1 ? 0 : doc_ + 2;
            return doc_;
        }
        [[nodiscard]] Result<int32_t> approx_advance(int32_t /*target*/) override { return doc_; }
        [[nodiscard]] Result<int64_t> approx_cost() const override { return int64_t{1}; }
        [[nodiscard]] Result<bool> matches() override { return true; }
        [[nodiscard]] float match_cost() const override { return 0.0F; }

    private:
        int32_t doc_{-1};
    };

    SlowTPI slow;
    EXPECT_EQ(slow.approx_slow_advance(5).value(), 6);
    EXPECT_EQ(slow.approx_slow_advance(1).value(), 6);
}

TEST(ScoreModeTest, ReportsWhatTheModeNeeds) {
    EXPECT_TRUE(score_mode_needs_scores(ScoreMode::Complete));
    EXPECT_TRUE(score_mode_needs_scores(ScoreMode::TopScores));
    EXPECT_TRUE(score_mode_needs_scores(ScoreMode::TopDocsWithScores));
    EXPECT_FALSE(score_mode_needs_scores(ScoreMode::CompleteNoScores));
    EXPECT_FALSE(score_mode_needs_scores(ScoreMode::TopDocs));

    EXPECT_TRUE(score_mode_is_exhaustive(ScoreMode::Complete));
    EXPECT_TRUE(score_mode_is_exhaustive(ScoreMode::CompleteNoScores));
    EXPECT_FALSE(score_mode_is_exhaustive(ScoreMode::TopScores));
    EXPECT_FALSE(score_mode_is_exhaustive(ScoreMode::TopDocs));
    EXPECT_FALSE(score_mode_is_exhaustive(ScoreMode::TopDocsWithScores));
}
