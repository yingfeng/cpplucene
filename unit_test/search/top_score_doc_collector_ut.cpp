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
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.top_score_doc_collector;

import lucene_core;

using namespace lucene;

namespace {

// ---------------------------------------------------------------------------
// Fakes: a query whose hits and scores are fixed, so the search loop, the
// collector and the merge can be exercised without a real postings format.
// ---------------------------------------------------------------------------

using Hit = std::pair<int32_t, float>;

/// Orders plain integers; enough to check the queue itself.
struct IntCompare {
    [[nodiscard]] Result<bool> less_than(int left, int right) const { return left < right; }
};

/// A scorer over a fixed list of `(doc, score)` pairs.
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

    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override {
        return std::numeric_limits<float>::infinity();
    }

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
};

class FixedScorerSupplier final : public ScorerSupplier {
public:
    explicit FixedScorerSupplier(std::vector<Hit> hits) : hits_(std::move(hits)) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        return std::unique_ptr<Scorer>(new FixedScorer(hits_));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return static_cast<int64_t>(hits_.size());
    }

private:
    std::vector<Hit> hits_;
};

class FixedQuery;

/// One weight over a per-leaf list of fixed hits.
class FixedWeight final : public Weight {
public:
    FixedWeight(const FixedQuery &query, std::vector<std::vector<Hit>> per_leaf)
        : query_(query), per_leaf_(std::move(per_leaf)) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext & /*context*/, int32_t /*doc*/,
                                              const IndexSearcher & /*searcher*/) const override {
        return Explanation::no_match_no_details("FixedWeight does not explain");
    }

    [[nodiscard]] const Query &get_query() const override;

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher & /*searcher*/) override {
        const auto ord = static_cast<size_t>(context.ord());
        if (ord >= per_leaf_.size() || per_leaf_[ord].empty()) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        return std::unique_ptr<ScorerSupplier>(new FixedScorerSupplier(per_leaf_[ord]));
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    const FixedQuery &query_;
    std::vector<std::vector<Hit>> per_leaf_;
};

/// A query whose hits are injected by the test.
class FixedQuery final : public Query {
public:
    explicit FixedQuery(std::vector<std::vector<Hit>> per_leaf) : per_leaf_(std::move(per_leaf)) {}

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher & /*searcher*/,
                                                                ScoreMode /*score_mode*/,
                                                                float /*boost*/) const override {
        return std::unique_ptr<Weight>(new FixedWeight(*this, per_leaf_));
    }

    [[nodiscard]] VoidResult visit(QueryVisitor & /*visitor*/) const override { return {}; }

    [[nodiscard]] Result<std::string> to_string(std::string_view /*field*/) const override { return "FixedQuery"; }

    [[nodiscard]] bool equals(const Query &other) const override { return this == &other; }

    [[nodiscard]] size_t hash() const override { return 42; }

private:
    std::vector<std::vector<Hit>> per_leaf_;
};

const Query &FixedWeight::get_query() const { return query_; }

/// A leaf reader with no postings, no doc values and no deletions: the search
/// loop only needs its `max_doc`, `live_docs` and identity.
class TestLeafReader final : public LeafReader {
public:
    explicit TestLeafReader(int32_t max_doc) : max_doc_(max_doc) {}

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return max_doc_; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override {
        return std::unique_ptr<StoredFields>{};
    }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override {
        return std::unique_ptr<TermVectors>{};
    }

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view /*field*/) override {
        return std::unique_ptr<Terms>{};
    }

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(
            std::string_view /*field*/) override {
        return std::unique_ptr<NumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<BinaryDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<SortedDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric_doc_values(
            std::string_view /*field*/) override {
        return std::unique_ptr<SortedNumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set_doc_values(
            std::string_view /*field*/) override {
        return std::unique_ptr<SortedSetDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view /*field*/) override {
        return std::unique_ptr<NumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(
            std::string_view /*field*/) override {
        return std::unique_ptr<DocValuesSkipper>{};
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    int32_t max_doc_;
    FieldInfos field_infos_{FieldInfos::none()};
};

/// A composite reader over the [`TestLeafReader`]s.
class TestCompositeReader final : public IndexReader {
public:
    explicit TestCompositeReader(std::vector<std::shared_ptr<LeafReader>> leaves) : leaves_(std::move(leaves)) {
        int32_t total = 0;
        for (const auto &leaf : leaves_) {
            total += leaf->max_doc().value();
        }
        max_doc_ = total;

        int32_t doc_base = 0;
        for (size_t i = 0; i < leaves_.size(); ++i) {
            const auto leaf_max = leaves_[i]->max_doc().value();
            contexts_.emplace_back(leaves_[i].get(), static_cast<int32_t>(i), doc_base,
                                   TopParentMeta{static_cast<int32_t>(leaves_.size()), total, identity()});
            doc_base += leaf_max;
        }
    }

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return max_doc_; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override {
        return std::unique_ptr<StoredFields>{};
    }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override {
        return std::unique_ptr<TermVectors>{};
    }

    [[nodiscard]] Result<int32_t> doc_freq(const Term & /*term*/) override { return 0; }
    [[nodiscard]] Result<int64_t> total_term_freq(const Term & /*term*/) override { return 0; }
    [[nodiscard]] Result<int64_t> sum_doc_freq(std::string_view /*field*/) override { return 0; }
    [[nodiscard]] Result<int32_t> doc_count(std::string_view /*field*/) override { return 0; }
    [[nodiscard]] Result<int64_t> sum_total_term_freq(std::string_view /*field*/) override { return 0; }

    [[nodiscard]] Result<std::span<const LeafReaderContext>> leaves() const override {
        return std::span<const LeafReaderContext>(contexts_);
    }

    [[nodiscard]] const Identity &identity() const override { return identity_; }

private:
    std::vector<std::shared_ptr<LeafReader>> leaves_;
    mutable std::vector<LeafReaderContext> contexts_;
    mutable Identity identity_{};
    int32_t max_doc_{0};
};

/// Counts every collected document; checks that `search_with_collector` feeds
/// the whole reader into one collector.
class CountingLeafCollector;

class CountingCollector final : public Collector {
public:
    [[nodiscard]] Result<std::unique_ptr<LeafCollector>>
    get_leaf_collector(const LeafReaderContext & /*context*/, const Weight * /*weight*/,
                       const IndexSearcher & /*searcher*/) override;

    [[nodiscard]] ScoreMode score_mode() const override { return ScoreMode::CompleteNoScores; }

    int32_t count{0};
};

class CountingLeafCollector final : public LeafCollector {
public:
    explicit CountingLeafCollector(CountingCollector &parent) : parent_(parent) {}

    [[nodiscard]] VoidResult collect(int32_t /*doc*/, Scorable & /*scorer*/) override {
        ++parent_.count;
        return {};
    }

private:
    CountingCollector &parent_;
};

Result<std::unique_ptr<LeafCollector>> CountingCollector::get_leaf_collector(const LeafReaderContext & /*context*/,
                                                                             const Weight * /*weight*/,
                                                                             const IndexSearcher & /*searcher*/) {
    return std::unique_ptr<LeafCollector>(new CountingLeafCollector(*this));
}

std::shared_ptr<IndexSearcher> make_searcher(std::vector<int32_t> leaf_sizes) {
    std::vector<std::shared_ptr<LeafReader>> leaves;
    leaves.reserve(leaf_sizes.size());
    for (const auto size : leaf_sizes) {
        leaves.push_back(std::make_shared<TestLeafReader>(size));
    }
    return std::make_shared<IndexSearcher>(std::make_shared<TestCompositeReader>(std::move(leaves)));
}

} // namespace

TEST(PriorityQueueTest, KeepsTheLeastElementOnTopAndPopsInOrder) {
    auto queue = PriorityQueue<int, IntCompare>::create(3, IntCompare{});
    ASSERT_TRUE(queue.has_value());
    auto pq = std::move(queue.value());

    EXPECT_EQ(pq.size(), 0U);
    EXPECT_EQ(pq.top(), nullptr);

    EXPECT_TRUE(pq.add(3).has_value());
    EXPECT_TRUE(pq.add(1).has_value());
    EXPECT_TRUE(pq.add(2).has_value());
    ASSERT_NE(pq.top(), nullptr);
    EXPECT_EQ(*pq.top(), 1);
    EXPECT_EQ(pq.size(), 3U);

    // Adding beyond the capacity is refused.
    auto overflow = pq.add(0);
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().kind(), ErrorKind::kArrayIndexOutOfBounds);

    EXPECT_EQ(pq.pop_unchecked().value(), 1);
    EXPECT_EQ(pq.pop_unchecked().value(), 2);
    EXPECT_EQ(pq.pop_unchecked().value(), 3);
    EXPECT_EQ(pq.size(), 0U);
    EXPECT_EQ(pq.pop_unchecked().error().kind(), ErrorKind::kIllegalState);
}

TEST(PriorityQueueTest, SentinelsStartTheQueueFull) {
    // A sentinel must compare worse than any real element: the queue keeps the
    // `max_size` least elements, so the sentinel is the smallest possible value.
    constexpr int kSentinel = -1000;
    auto queue = PriorityQueue<int, IntCompare>::with_sentinel_object(
            1, [kSentinel]() -> std::optional<int> { return kSentinel; }, IntCompare{});
    ASSERT_TRUE(queue.has_value());
    auto pq = std::move(queue.value());

    // The reference implementation starts with `max_size` sentinel elements, so the queue is full.
    EXPECT_EQ(pq.size(), 1U);
    ASSERT_NE(pq.top(), nullptr);
    EXPECT_EQ(*pq.top(), kSentinel);

    // Inserting overflows: the worst element is evicted, which is the sentinel.
    auto first = pq.insert_with_overflow(5);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first.value().has_value());
    EXPECT_EQ(first.value().value(), kSentinel);
    ASSERT_NE(pq.top(), nullptr);
    EXPECT_EQ(*pq.top(), 5);

    // A better element replaces the top, a worse one is dropped.
    auto better = pq.insert_with_overflow(9);
    ASSERT_TRUE(better.has_value());
    ASSERT_TRUE(better.value().has_value());
    EXPECT_EQ(better.value().value(), 5);
    EXPECT_EQ(*pq.top(), 9);

    auto worse = pq.insert_with_overflow(3);
    ASSERT_TRUE(worse.has_value());
    ASSERT_TRUE(worse.value().has_value());
    EXPECT_EQ(worse.value().value(), 3);
    EXPECT_EQ(*pq.top(), 9);

    // Writing the top and re-heapifying is the collector's fast path.
    *pq.top_mut() = 1;
    auto updated = pq.update_top();
    ASSERT_TRUE(updated.has_value());
    ASSERT_NE(*updated, nullptr);
    EXPECT_EQ(**updated, 1);
}

TEST(PriorityQueueTest, AddAllBuildsTheHeap) {
    auto queue = PriorityQueue<int, IntCompare>::create(4, IntCompare{});
    ASSERT_TRUE(queue.has_value());
    auto pq = std::move(queue.value());

    ASSERT_TRUE(pq.add_all({4, 2, 3, 1}).has_value());
    ASSERT_NE(pq.top(), nullptr);
    EXPECT_EQ(*pq.top(), 1);
    EXPECT_EQ(pq.size(), 4U);

    auto taken = pq.take_all();
    EXPECT_EQ(taken.size(), 4U);
    EXPECT_EQ(pq.size(), 0U);
}

TEST(HitQueueTest, RanksLowerScoresFirstAndHigherDocsOnATie) {
    const HitQueueComparator compare;
    const auto best = ScoreDoc::create(2, 5.0F);
    const auto worse = ScoreDoc::create(1, 3.0F);
    const auto tie = ScoreDoc::create(3, 3.0F);

    EXPECT_TRUE(compare.less_than(worse, best).value());
    EXPECT_FALSE(compare.less_than(best, worse).value());
    // Equal scores: the higher document number is the less competitive hit.
    EXPECT_TRUE(compare.less_than(tie, worse).value());
    EXPECT_FALSE(compare.less_than(worse, tie).value());
}

TEST(TotalHitsTest, FormatsExactAndLowerBoundCounts) {
    EXPECT_EQ(TotalHits::create(7, TotalHits::Relation::EqualTo).to_string(), "7 hits");
    EXPECT_EQ(TotalHits::create(7, TotalHits::Relation::GreaterThanOrEqualTo).to_string(), "7+ hits");
    EXPECT_EQ(ScoreDoc::create(3, 1.5F).to_string(), "doc=3 score=1.5 shardIndex=-1");
}

TEST(TopDocsTest, MergeInterleavesShardsByScore) {
    const TopDocs shard0 = TopDocs::create(TotalHits::create(2, TotalHits::Relation::EqualTo),
                                           {ScoreDoc::create(0, 5.0F), ScoreDoc::create(1, 2.0F)});
    const TopDocs shard1 = TopDocs::create(TotalHits::create(2, TotalHits::Relation::GreaterThanOrEqualTo),
                                           {ScoreDoc::create(0, 4.0F), ScoreDoc::create(1, 3.0F)});

    auto merged = merge_top_docs(3, {shard0, shard1});
    ASSERT_TRUE(merged.has_value());
    ASSERT_EQ(merged.value().score_docs.size(), 3U);
    EXPECT_EQ(merged.value().score_docs[0].score, 5.0F);
    EXPECT_EQ(merged.value().score_docs[1].score, 4.0F);
    EXPECT_EQ(merged.value().score_docs[2].score, 3.0F);
    // The lower-bound relation of any shard wins.
    EXPECT_EQ(merged.value().total_hits.value, 4U);
    EXPECT_EQ(merged.value().total_hits.relation, TotalHits::Relation::GreaterThanOrEqualTo);
}

TEST(TopDocsTest, MergeSkipsTheFirstHitsForPagination) {
    const TopDocs shard0 = TopDocs::create(TotalHits::create(3, TotalHits::Relation::EqualTo),
                                           {ScoreDoc::create(0, 9.0F), ScoreDoc::create(1, 6.0F),
                                            ScoreDoc::create(2, 3.0F)});

    auto merged = merge_top_docs_with_start(1, 1, {shard0});
    ASSERT_TRUE(merged.has_value());
    ASSERT_EQ(merged.value().score_docs.size(), 1U);
    EXPECT_EQ(merged.value().score_docs[0].score, 6.0F);
    EXPECT_EQ(merged.value().total_hits.value, 3U);
}

TEST(TopDocsTest, MergeRejectsInconsistentShardIndices) {
    const TopDocs shard0 = TopDocs::create(TotalHits::create(1, TotalHits::Relation::EqualTo),
                                           {ScoreDoc::with_shard_index(0, 5.0F, 0)});
    const TopDocs shard1 = TopDocs::create(TotalHits::create(1, TotalHits::Relation::EqualTo),
                                           {ScoreDoc::create(0, 4.0F)});

    auto merged = merge_top_docs(2, {shard0, shard1});
    ASSERT_FALSE(merged.has_value());
    EXPECT_EQ(merged.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(IndexSearcherSearchTest, ReturnsTheTopHitsOfEveryLeaf) {
    auto searcher = make_searcher({2, 2});
    FixedQuery query({{Hit{0, 5.0F}, Hit{1, 3.0F}}, {Hit{0, 4.0F}}});

    auto top_docs = searcher->search(query, 10);
    ASSERT_TRUE(top_docs.has_value());
    ASSERT_EQ(top_docs.value().score_docs.size(), 3U);

    // Hits are sorted by score, and the second leaf's document number is offset
    // by the first leaf's max_doc.
    EXPECT_EQ(top_docs.value().score_docs[0].doc, 0);
    EXPECT_EQ(top_docs.value().score_docs[1].doc, 2);
    EXPECT_EQ(top_docs.value().score_docs[2].doc, 1);
    EXPECT_EQ(top_docs.value().score_docs[1].score, 4.0F);
    EXPECT_EQ(top_docs.value().total_hits.value, 3U);
    EXPECT_EQ(top_docs.value().total_hits.relation, TotalHits::Relation::EqualTo);
}

TEST(IndexSearcherSearchTest, ZeroRequestedHitsStillCountsTheMatches) {
    auto searcher = make_searcher({3});
    FixedQuery query({{Hit{0, 5.0F}, Hit{1, 4.0F}, Hit{2, 3.0F}}});

    auto top_docs = searcher->search(query, 0);
    ASSERT_TRUE(top_docs.has_value());
    EXPECT_TRUE(top_docs.value().score_docs.empty());
    EXPECT_EQ(top_docs.value().total_hits.value, 3U);
}

TEST(IndexSearcherSearchTest, LowTotalHitsThresholdReportsALowerBound) {
    auto searcher = make_searcher({3});
    FixedQuery query({{Hit{0, 5.0F}, Hit{1, 4.0F}, Hit{2, 3.0F}}});

    auto manager = TopScoreDocCollectorManager::create(/*num_hits=*/1, /*total_hits_threshold=*/1);
    ASSERT_TRUE(manager.has_value());

    auto top_docs = searcher->search_with_collector_manager(query, *manager.value());
    ASSERT_TRUE(top_docs.has_value());
    ASSERT_EQ(top_docs.value().score_docs.size(), 1U);
    EXPECT_EQ(top_docs.value().score_docs[0].doc, 0);
    EXPECT_EQ(top_docs.value().score_docs[0].score, 5.0F);
    EXPECT_EQ(top_docs.value().total_hits.value, 3U);
    EXPECT_EQ(top_docs.value().total_hits.relation, TotalHits::Relation::GreaterThanOrEqualTo);
}

TEST(IndexSearcherSearchTest, SearchAfterSkipsThePreviousPage) {
    auto searcher = make_searcher({4});
    FixedQuery query({{Hit{0, 5.0F}, Hit{1, 4.0F}, Hit{2, 3.0F}, Hit{3, 2.0F}}});

    auto first_page = searcher->search(query, 2);
    ASSERT_TRUE(first_page.has_value());
    ASSERT_EQ(first_page.value().score_docs.size(), 2U);

    const ScoreDoc cursor = first_page.value().score_docs.back();
    EXPECT_EQ(cursor.doc, 1);

    auto second_page = searcher->search_after_score(&cursor, query, 2);
    ASSERT_TRUE(second_page.has_value());
    ASSERT_EQ(second_page.value().score_docs.size(), 2U);
    EXPECT_EQ(second_page.value().score_docs[0].doc, 2);
    EXPECT_EQ(second_page.value().score_docs[0].score, 3.0F);
    EXPECT_EQ(second_page.value().score_docs[1].doc, 3);
    // Skipped hits are still counted, like the reference implementation does.
    EXPECT_EQ(second_page.value().total_hits.value, 4U);
}

TEST(IndexSearcherSearchTest, TopDocsWithStartPaginatesTheCollectedQueue) {
    auto searcher = make_searcher({4});
    FixedQuery query({{Hit{0, 8.0F}, Hit{1, 6.0F}, Hit{2, 4.0F}, Hit{3, 2.0F}}});

    auto collector = TopScoreDocCollector::create(/*num_hits=*/4, std::nullopt, 1000, nullptr);
    ASSERT_TRUE(collector.has_value());

    ASSERT_TRUE(searcher->search_with_collector(query, *collector.value()).has_value());

    auto page = collector.value()->top_docs_with_start(1);
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page.value().score_docs.size(), 3U);
    EXPECT_EQ(page.value().score_docs[0].score, 6.0F);
    EXPECT_EQ(page.value().score_docs[2].score, 2.0F);
}

TEST(IndexSearcherSearchTest, SearchWithCollectorFeedsEveryLeaf) {
    auto searcher = make_searcher({2, 3});
    FixedQuery query({{Hit{0, 5.0F}, Hit{1, 3.0F}}, {Hit{0, 4.0F}, Hit{2, 1.0F}}});

    CountingCollector collector;
    ASSERT_TRUE(searcher->search_with_collector(query, collector).has_value());
    EXPECT_EQ(collector.count, 4);
}

TEST(IndexSearcherSearchTest, RejectsANegativeHitCount) {
    auto searcher = make_searcher({1});
    FixedQuery query({{Hit{0, 1.0F}}});

    auto top_docs = searcher->search(query, -1);
    ASSERT_FALSE(top_docs.has_value());
    EXPECT_EQ(top_docs.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(MaxScoreAccumulatorTest, KeepsTheBestScoreAndItsDocument) {
    MaxScoreAccumulator acc;
    EXPECT_EQ(acc.get_raw(), std::numeric_limits<int64_t>::min());
    EXPECT_EQ(acc.mod_interval(), MaxScoreAccumulator::DEFAULT_INTERVAL);

    acc.accumulate(3, 2.5F);
    EXPECT_EQ(MaxScoreAccumulator::to_score(acc.get_raw()), 2.5F);
    EXPECT_EQ(MaxScoreAccumulator::doc_id(acc.get_raw()), 3);

    // A worse score changes nothing.
    acc.accumulate(1, 1.0F);
    EXPECT_EQ(MaxScoreAccumulator::doc_id(acc.get_raw()), 3);

    // A better score wins.
    acc.accumulate(7, 4.0F);
    EXPECT_EQ(MaxScoreAccumulator::to_score(acc.get_raw()), 4.0F);
    EXPECT_EQ(MaxScoreAccumulator::doc_id(acc.get_raw()), 7);

    // A tie keeps the smaller document id, i.e. the first hit in document order.
    acc.accumulate(5, 4.0F);
    EXPECT_EQ(MaxScoreAccumulator::doc_id(acc.get_raw()), 5);
    EXPECT_EQ(MaxScoreAccumulator::to_score(acc.get_raw()), 4.0F);
}
