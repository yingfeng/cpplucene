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
#include <optional>
#include <span>
#include <vector>

module lucene_core:ut.search_utils;

import lucene_core;

using namespace lucene;

namespace {

/// A scorer over `[0, max_doc)` that always reports the score `1`.
class RangeScorer final : public Scorer {
public:
    explicit RangeScorer(int32_t max_doc) : max_doc_(max_doc) {}

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
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }
    [[nodiscard]] Result<float> score() override { return 1.0F; }

private:
    int32_t max_doc_{0};
    int32_t doc_{-1};
};

/// Keeps the even documents of a range.
class EvenDocsIterator final : public FilteredDocIdSetIterator {
public:
    explicit EvenDocsIterator(int32_t max_doc) : FilteredDocIdSetIterator(std::make_unique<RangeScorer>(max_doc)) {}

protected:
    [[nodiscard]] Result<bool> match(int32_t doc) override { return doc % 2 == 0; }
};

/// A scorer over `[0, max_doc)` whose even documents score 0 and whose odd
/// documents score 2, so that the positive-scores wrapper has something to drop.
class ParityScorer final : public Scorer {
public:
    explicit ParityScorer(int32_t max_doc) : max_doc_(max_doc) {}

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
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override { return 2.0F; }
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }
    [[nodiscard]] Result<float> score() override { return doc_ % 2 == 0 ? 0.0F : 2.0F; }

private:
    int32_t max_doc_{0};
    int32_t doc_{-1};
};

class ParityScorerSupplier final : public ScorerSupplier {
public:
    explicit ParityScorerSupplier(int32_t max_doc) : max_doc_(max_doc) {}
    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        return std::unique_ptr<Scorer>(new ParityScorer(max_doc_));
    }
    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return max_doc_;
    }

private:
    int32_t max_doc_{0};
};

class ParityQuery;

class ParityWeight final : public Weight {
public:
    explicit ParityWeight(const ParityQuery &query) : query_(query) {}
    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext & /*context*/, int32_t /*doc*/,
                                              const IndexSearcher & /*searcher*/) const override {
        return Explanation::no_match_no_details("ParityWeight does not explain");
    }
    [[nodiscard]] const Query &get_query() const override;
    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext & context,
                                                                         const IndexSearcher & /*searcher*/) override {
        return std::unique_ptr<ScorerSupplier>(
                std::make_unique<ParityScorerSupplier>(context.reader()->max_doc().value()));
    }
    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    const ParityQuery &query_;
};

class ParityQuery final : public Query {
public:
    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher & /*searcher*/,
                                                               ScoreMode /*score_mode*/,
                                                               float /*boost*/) const override {
        return std::unique_ptr<Weight>(new ParityWeight(*this));
    }
    [[nodiscard]] VoidResult visit(QueryVisitor & /*visitor*/) const override { return {}; }
    [[nodiscard]] Result<std::string> to_string(std::string_view /*field*/) const override {
        return std::string("Parity");
    }
    [[nodiscard]] bool equals(const Query &other) const override {
        return dynamic_cast<const ParityQuery *>(&other) != nullptr;
    }
    [[nodiscard]] size_t hash() const override { return 7; }
};

const Query &ParityWeight::get_query() const { return query_; }

/// A weight that claims to know how many documents match, so that the hit count
/// collector skips the leaf.
class CountableQuery;

class CountableWeight final : public Weight {
public:
    explicit CountableWeight(const CountableQuery &query) : query_(query) {}
    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext & /*context*/, int32_t /*doc*/,
                                              const IndexSearcher & /*searcher*/) const override {
        return Explanation::no_match_no_details("CountableWeight does not explain");
    }
    [[nodiscard]] const Query &get_query() const override;
    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext & /*context*/,
                                                                         const IndexSearcher & /*searcher*/) override {
        return std::unique_ptr<ScorerSupplier>(std::make_unique<ParityScorerSupplier>(0));
    }
    [[nodiscard]] Result<int32_t> count(const LeafReaderContext & /*context*/,
                                        const IndexSearcher & /*searcher*/) override {
        // The answer the collector has to use instead of visiting the leaf.
        return 2;
    }
    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    const CountableQuery &query_;
};

class CountableQuery final : public Query {
public:
    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher & /*searcher*/,
                                                               ScoreMode /*score_mode*/,
                                                               float /*boost*/) const override {
        return std::unique_ptr<Weight>(new CountableWeight(*this));
    }
    [[nodiscard]] VoidResult visit(QueryVisitor & /*visitor*/) const override { return {}; }
    [[nodiscard]] Result<std::string> to_string(std::string_view /*field*/) const override {
        return std::string("Countable");
    }
    [[nodiscard]] bool equals(const Query &other) const override {
        return dynamic_cast<const CountableQuery *>(&other) != nullptr;
    }
    [[nodiscard]] size_t hash() const override { return 11; }
};

const Query &CountableWeight::get_query() const { return query_; }

/// Records the documents it is handed.
class CapturingCollector final : public Collector {
public:
    class Leaf final : public LeafCollector {
    public:
        explicit Leaf(std::vector<int32_t> &docs) : docs_(&docs) {}
        [[nodiscard]] VoidResult collect(int32_t doc, Scorable & /*scorer*/) override {
            docs_->push_back(doc);
            return {};
        }

    private:
        std::vector<int32_t> *docs_{nullptr};
    };

    [[nodiscard]] Result<std::unique_ptr<LeafCollector>> get_leaf_collector(
            const LeafReaderContext & /*context*/, const Weight * /*weight*/,
            const IndexSearcher & /*searcher*/) override {
        return std::unique_ptr<LeafCollector>(new Leaf(docs));
    }
    [[nodiscard]] ScoreMode score_mode() const override { return ScoreMode::Complete; }

    std::vector<int32_t> docs;
};

/// A leaf with four documents and no doc values.
class EmptyLeafReader final : public LeafReader {
public:
    [[nodiscard]] Result<int32_t> max_doc() const override { return 4; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return 4; }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view /*field*/) override {
        return std::unique_ptr<Terms>{};
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric_doc_values(
            std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set_doc_values(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    FieldInfos field_infos_{FieldInfos::none()};
};

IndexSearcher make_searcher() {
    std::shared_ptr<IndexReader> reader = std::make_shared<EmptyLeafReader>();
    return IndexSearcher(std::move(reader));
}

} // namespace

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------

TEST(SearchUtilsTest, ScoreReportsAndAcceptsAFixedScore) {
    Score score(2.5F);
    ASSERT_TRUE(score.score().has_value());
    EXPECT_FLOAT_EQ(score.score().value(), 2.5F);
    EXPECT_FLOAT_EQ(score.value(), 2.5F);

    ASSERT_TRUE(score.set_score(4.0F).has_value());
    EXPECT_FLOAT_EQ(score.score().value(), 4.0F);
}

// ---------------------------------------------------------------------------
// DisiWrapper
// ---------------------------------------------------------------------------

TEST(SearchUtilsTest, DisiWrapperCachesCostAndMatchCost) {
    auto wrapper = DisiWrapper<RangeScorer>::create(RangeScorer(7));
    ASSERT_TRUE(wrapper.has_value()) << wrapper.error().to_string();
    EXPECT_EQ(wrapper.value().cost(), 7);
    EXPECT_FLOAT_EQ(wrapper.value().match_cost(), 0.0F);
    EXPECT_EQ(wrapper.value().doc(), -1);

    wrapper.value().set_doc(3);
    EXPECT_EQ(wrapper.value().doc(), 3);
    wrapper.value().set_next(4);
    ASSERT_TRUE(wrapper.value().next().has_value());
    EXPECT_EQ(wrapper.value().next().value(), 4);
    wrapper.value().set_scaled_max_score(11);
    EXPECT_EQ(wrapper.value().scaled_max_score(), 11);
    wrapper.value().set_max_window_score(2.0F);
    EXPECT_FLOAT_EQ(wrapper.value().max_window_score(), 2.0F);
}

TEST(SearchUtilsTest, DisiWrapperRejectsMatchesWithoutATwoPhaseIterator) {
    auto wrapper = DisiWrapper<RangeScorer>::create(RangeScorer(7));
    ASSERT_TRUE(wrapper.has_value());
    auto matches = wrapper.value().matches();
    ASSERT_FALSE(matches.has_value());
    EXPECT_EQ(matches.error().kind(), ErrorKind::kIllegalState);

    // The lenient entry point just asks the scorer.
    auto lenient = wrapper.value().matches_may_none();
    ASSERT_TRUE(lenient.has_value());
    EXPECT_TRUE(lenient.value());
}

// ---------------------------------------------------------------------------
// ScorerUtil
// ---------------------------------------------------------------------------

TEST(SearchUtilsTest, CostWithMinShouldMatchSumsTheCheapestClauses) {
    const std::vector<int64_t> costs{1, 2, 3};

    // Every clause may be needed: the cost is the whole sum.
    auto all = ScorerUtil::cost_with_min_should_match(costs, 3, 1);
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all.value(), 6);

    // Two of three have to match: the two cheapest clauses.
    auto two = ScorerUtil::cost_with_min_should_match(costs, 3, 2);
    ASSERT_TRUE(two.has_value());
    EXPECT_EQ(two.value(), 3);

    // All of them have to match: only the cheapest one is guaranteed.
    auto three = ScorerUtil::cost_with_min_should_match(costs, 3, 3);
    ASSERT_TRUE(three.has_value());
    EXPECT_EQ(three.value(), 1);
}

TEST(SearchUtilsTest, CostWithMinShouldMatchHandlesSingleAndUnsortedCosts) {
    const std::vector<int64_t> single{5};
    auto one = ScorerUtil::cost_with_min_should_match(single, 1, 1);
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one.value(), 5);

    const std::vector<int64_t> unsorted{9, 2, 7, 4};
    auto cheapest_two = ScorerUtil::cost_with_min_should_match(unsorted, 4, 3);
    ASSERT_TRUE(cheapest_two.has_value());
    EXPECT_EQ(cheapest_two.value(), 6);
}

TEST(SearchUtilsTest, CostWithMinShouldMatchRejectsAnImpossibleCount) {
    const std::vector<int64_t> costs{1, 2};
    auto invalid = ScorerUtil::cost_with_min_should_match(costs, 2, 3);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().kind(), ErrorKind::kIllegalArgument);
}

// ---------------------------------------------------------------------------
// FilteredDocIdSetIterator
// ---------------------------------------------------------------------------

TEST(SearchUtilsTest, FilteredIteratorSkipsTheRejectedDocuments) {
    EvenDocsIterator iterator(6);
    std::vector<int32_t> docs;
    auto doc = iterator.next_doc();
    while (doc.has_value() && doc.value() != NO_MORE_DOCS) {
        docs.push_back(doc.value());
        doc = iterator.next_doc();
    }
    EXPECT_EQ(docs, (std::vector<int32_t>{0, 2, 4}));
    EXPECT_EQ(iterator.cost().value(), 6);
}

TEST(SearchUtilsTest, FilteredIteratorAdvancesToTheNextAcceptedDocument) {
    EvenDocsIterator iterator(8);
    auto first = iterator.advance(3);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first.value(), 4);
    EXPECT_EQ(iterator.doc_id(), 4);

    auto second = iterator.advance(5);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value(), 6);

    auto exhausted = iterator.advance(100);
    ASSERT_TRUE(exhausted.has_value());
    EXPECT_EQ(exhausted.value(), NO_MORE_DOCS);
}

// ---------------------------------------------------------------------------
// Pruning
// ---------------------------------------------------------------------------

TEST(SearchUtilsTest, PruningValuesMatchTheReferenceOrder) {
    EXPECT_LT(static_cast<int32_t>(Pruning::kNone), static_cast<int32_t>(Pruning::kGreaterThan));
    EXPECT_LT(static_cast<int32_t>(Pruning::kGreaterThan),
              static_cast<int32_t>(Pruning::kGreaterThanOrEqualTo));
}

// ---------------------------------------------------------------------------
// collectors
// ---------------------------------------------------------------------------

TEST(SearchUtilsTest, TotalHitCountCollectorCountsEveryHit) {
    const IndexSearcher searcher = make_searcher();
    auto counted = searcher.count(ParityQuery());
    ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
    EXPECT_EQ(counted.value(), 4);
}

TEST(SearchUtilsTest, TotalHitCountCollectorUsesTheWeightsCount) {
    const IndexSearcher searcher = make_searcher();
    auto counted = searcher.count(CountableQuery());
    ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
    // The weight answered 2, so the leaf was never visited.
    EXPECT_EQ(counted.value(), 2);
}

TEST(SearchUtilsTest, PositiveScoresOnlyCollectorDropsZeroScores) {
    const IndexSearcher searcher = make_searcher();
    CapturingCollector capturing;
    PositiveScoresOnlyCollector collecting(capturing);
    auto collected = searcher.search_with_collector(ParityQuery(), collecting);
    ASSERT_TRUE(collected.has_value()) << collected.error().to_string();
    EXPECT_EQ(capturing.docs, (std::vector<int32_t>{1, 3}));
}

TEST(SearchUtilsTest, PositiveScoresOnlyCollectorNeedsScores) {
    CapturingCollector capturing;
    PositiveScoresOnlyCollector collecting(capturing);
    // The wrapped collector is exhaustive, so every match is visited.
    EXPECT_EQ(collecting.score_mode(), ScoreMode::Complete);
}

TEST(SearchUtilsTest, MultiCollectorFeedsEveryCollector) {
    const IndexSearcher searcher = make_searcher();
    CapturingCollector first;
    CapturingCollector second;
    std::vector<Collector *> inner{&first, &second};
    auto collecting = MultiCollector::wrap(std::move(inner));
    ASSERT_TRUE(collecting.has_value()) << collecting.error().to_string();
    auto collected = searcher.search_with_collector(ParityQuery(), *collecting.value());
    ASSERT_TRUE(collected.has_value()) << collected.error().to_string();
    EXPECT_EQ(first.docs, (std::vector<int32_t>{0, 1, 2, 3}));
    EXPECT_EQ(second.docs, (std::vector<int32_t>{0, 1, 2, 3}));
    EXPECT_EQ(collecting.value()->score_mode(), ScoreMode::Complete);
}

TEST(SearchUtilsTest, MultiCollectorRejectsAnEmptyList) {
    auto collecting = MultiCollector::wrap({});
    ASSERT_FALSE(collecting.has_value());
    EXPECT_EQ(collecting.error().kind(), ErrorKind::kIllegalArgument);
}
