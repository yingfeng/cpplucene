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
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.boolean_query;

import lucene_core;

using namespace lucene;

namespace {

using Hit = std::pair<int32_t, float>;

/// A scorer over fixed `(doc, score)` pairs.
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
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override { return 1.0F; }
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

/// A query with a test-provided name and hits.
class FixedQuery final : public Query {
public:
    FixedQuery(std::string name, std::vector<std::vector<Hit>> per_leaf)
        : name_(std::move(name)), per_leaf_(std::move(per_leaf)) {}

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher & /*searcher*/,
                                                                ScoreMode /*score_mode*/,
                                                                float /*boost*/) const override {
        return std::unique_ptr<Weight>(new FixedWeight(*this, per_leaf_));
    }

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override { return visitor.visit_leaf(*this); }

    [[nodiscard]] Result<std::string> to_string(std::string_view /*field*/) const override { return name_; }

    [[nodiscard]] bool equals(const Query &other) const override {
        const auto *that = dynamic_cast<const FixedQuery *>(&other);
        return that != nullptr && that->name_ == name_;
    }

    [[nodiscard]] size_t hash() const override { return std::hash<std::string>{}(name_); }

private:
    std::string name_;
    std::vector<std::vector<Hit>> per_leaf_;
};

const Query &FixedWeight::get_query() const { return query_; }

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

std::shared_ptr<IndexSearcher> make_searcher(int32_t num_docs) {
    std::vector<std::shared_ptr<LeafReader>> leaves;
    leaves.push_back(std::make_shared<TestLeafReader>(num_docs));
    return std::make_shared<IndexSearcher>(std::make_shared<TestCompositeReader>(std::move(leaves)));
}

std::shared_ptr<Query> fixed(std::string name, std::vector<Hit> hits) {
    return std::make_shared<FixedQuery>(std::move(name), std::vector<std::vector<Hit>>{std::move(hits)});
}

Result<BooleanQuery> boolean(std::vector<std::pair<std::shared_ptr<Query>, Occur>> clauses, int32_t min_should = 0) {
    BooleanQuery::Builder builder;
    builder.set_minimum_number_should_match(min_should);
    for (auto &[query, occur] : clauses) {
        if (auto added = builder.add_clause(BooleanClause::create(std::move(query), occur)); !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return builder.build();
}

std::vector<int32_t> docs_of(const TopDocs &top_docs) {
    std::vector<int32_t> docs;
    docs.reserve(top_docs.score_docs.size());
    for (const ScoreDoc &hit : top_docs.score_docs) {
        docs.push_back(hit.doc);
    }
    return docs;
}

} // namespace

TEST(BooleanQueryTest, MustDecidesTheDocumentSetAndShouldAddsScore) {
    auto searcher = make_searcher(4);
    auto must = fixed("a", {{0, 1.0F}, {1, 2.0F}, {2, 3.0F}});
    auto should = fixed("b", {{1, 10.0F}});

    auto query = boolean({{must, Occur::Must}, {should, Occur::Should}});
    ASSERT_TRUE(query.has_value());
    auto top_docs = searcher->search(query.value(), 10);
    ASSERT_TRUE(top_docs.has_value());

    // Ordered by score: 12 (doc 1), 3 (doc 2), 1 (doc 0).
    EXPECT_EQ(docs_of(top_docs.value()), (std::vector<int32_t>{1, 2, 0}));
    EXPECT_EQ(top_docs.value().score_docs[0].score, 12.0F);
    EXPECT_EQ(top_docs.value().score_docs[1].score, 3.0F);
    EXPECT_EQ(top_docs.value().score_docs[2].score, 1.0F);
}

TEST(BooleanQueryTest, FilterClauseConstrainsWithoutScoring) {
    auto searcher = make_searcher(4);
    auto must = fixed("a", {{0, 5.0F}, {1, 5.0F}, {2, 5.0F}});
    auto filter = fixed("b", {{0, 100.0F}, {2, 100.0F}});

    auto query = boolean({{must, Occur::Must}, {filter, Occur::Filter}});
    ASSERT_TRUE(query.has_value());
    auto top_docs = searcher->search(query.value(), 10);
    ASSERT_TRUE(top_docs.has_value());

    EXPECT_EQ(docs_of(top_docs.value()), (std::vector<int32_t>{0, 2}));
    // The filter's score never leaks out.
    EXPECT_EQ(top_docs.value().score_docs[0].score, 5.0F);
    EXPECT_EQ(top_docs.value().score_docs[1].score, 5.0F);
}

TEST(BooleanQueryTest, MustNotExcludesDocuments) {
    auto searcher = make_searcher(4);
    auto must = fixed("a", {{0, 1.0F}, {1, 1.0F}, {2, 1.0F}});
    auto excluded = fixed("b", {{1, 1.0F}});

    auto query = boolean({{must, Occur::Must}, {excluded, Occur::MustNot}});
    ASSERT_TRUE(query.has_value());
    auto top_docs = searcher->search(query.value(), 10);
    ASSERT_TRUE(top_docs.has_value());
    EXPECT_EQ(docs_of(top_docs.value()), (std::vector<int32_t>{0, 2}));
}

TEST(BooleanQueryTest, PureShouldIsADisjunction) {
    auto searcher = make_searcher(4);
    auto first = fixed("a", {{0, 1.0F}, {1, 2.0F}});
    auto second = fixed("b", {{1, 3.0F}, {2, 4.0F}});

    auto query = boolean({{first, Occur::Should}, {second, Occur::Should}});
    ASSERT_TRUE(query.has_value());
    auto top_docs = searcher->search(query.value(), 10);
    ASSERT_TRUE(top_docs.has_value());

    // Document 1 matches both clauses, so its score (5) beats document 2 (4).
    EXPECT_EQ(docs_of(top_docs.value()), (std::vector<int32_t>{1, 2, 0}));
    EXPECT_EQ(top_docs.value().score_docs[0].score, 5.0F);
    EXPECT_EQ(top_docs.value().score_docs[1].score, 4.0F);
}

TEST(BooleanQueryTest, MinimumShouldMatchRequiresEnoughClauses) {
    auto searcher = make_searcher(4);
    auto first = fixed("a", {{0, 1.0F}, {1, 1.0F}, {2, 1.0F}});
    auto second = fixed("b", {{1, 1.0F}, {2, 1.0F}});
    auto third = fixed("c", {{2, 1.0F}});

    auto query = boolean({{first, Occur::Should}, {second, Occur::Should}, {third, Occur::Should}}, /*min_should=*/2);
    ASSERT_TRUE(query.has_value());
    auto top_docs = searcher->search(query.value(), 10);
    ASSERT_TRUE(top_docs.has_value());

    // Only documents 1 (two clauses, score 2) and 2 (three clauses, score 3) are
    // matched by at least two clauses.
    EXPECT_EQ(docs_of(top_docs.value()), (std::vector<int32_t>{2, 1}));
    EXPECT_EQ(top_docs.value().score_docs[0].score, 3.0F);
    EXPECT_EQ(top_docs.value().score_docs[1].score, 2.0F);
}

TEST(BooleanQueryTest, MinimumShouldMatchTurnsShouldIntoAConjunction) {
    auto searcher = make_searcher(4);
    auto must = fixed("a", {{0, 1.0F}, {1, 1.0F}, {2, 1.0F}});
    auto first = fixed("b", {{1, 1.0F}, {2, 1.0F}});
    auto second = fixed("c", {{2, 1.0F}});

    auto query = boolean({{must, Occur::Must}, {first, Occur::Should}, {second, Occur::Should}}, /*min_should=*/2);
    ASSERT_TRUE(query.has_value());
    auto top_docs = searcher->search(query.value(), 10);
    ASSERT_TRUE(top_docs.has_value());

    // Document 2 is the only one required *and* matching two optional clauses.
    EXPECT_EQ(docs_of(top_docs.value()), (std::vector<int32_t>{2}));
    EXPECT_EQ(top_docs.value().score_docs[0].score, 3.0F);
}

TEST(BooleanQueryTest, EmptyQueryMatchesNothing) {
    auto searcher = make_searcher(3);
    auto query = BooleanQuery::create({});
    ASSERT_TRUE(query.has_value());

    auto top_docs = searcher->search(query.value(), 10);
    ASSERT_TRUE(top_docs.has_value());
    EXPECT_TRUE(top_docs.value().score_docs.empty());
    EXPECT_EQ(top_docs.value().total_hits.value, 0U);
}

TEST(BooleanQueryTest, PureNegativeQueryMatchesNothing) {
    auto searcher = make_searcher(3);
    auto excluded = fixed("a", {{0, 1.0F}});
    auto query = boolean({{excluded, Occur::MustNot}});
    ASSERT_TRUE(query.has_value());

    auto top_docs = searcher->search(query.value(), 10);
    ASSERT_TRUE(top_docs.has_value());
    EXPECT_TRUE(top_docs.value().score_docs.empty());
}

TEST(BooleanQueryTest, GenericCountUsesTheStatistics) {
    auto searcher = make_searcher(3);
    auto match_all = MatchAllDocsQuery::create();
    auto excluded = MatchAllDocsQuery::create();
    auto query = boolean({{match_all, Occur::Must}, {excluded, Occur::MustNot}});
    ASSERT_TRUE(query.has_value());

    auto weight = query.value().create_weight(*searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher->leaves();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_EQ(leaves.value().size(), 1U);
    auto count = weight.value()->count(leaves.value()[0], *searcher);
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 0);
}

TEST(BooleanQueryTest, MatchAllDocsMatchesEveryDocument) {
    auto searcher = make_searcher(3);
    MatchAllDocsQuery query;

    auto top_docs = searcher->search(query, 10);
    ASSERT_TRUE(top_docs.has_value());
    ASSERT_EQ(top_docs.value().score_docs.size(), 3U);
    EXPECT_EQ(top_docs.value().total_hits.value, 3U);
    EXPECT_EQ(top_docs.value().score_docs[0].score, 1.0F);
    EXPECT_EQ(docs_of(top_docs.value()), (std::vector<int32_t>{0, 1, 2}));
}

TEST(BooleanQueryTest, ToStringShowsOccurrencesAndMinimumShouldMatch) {
    auto must = fixed("a", {});
    auto should = fixed("b", {});
    auto excluded = fixed("c", {});
    auto nested = fixed("d", {});

    auto query = boolean({{must, Occur::Must}, {should, Occur::Should}, {excluded, Occur::MustNot}});
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(query.value().to_string("").value(), "+a b -c");

    auto with_min = boolean({{must, Occur::Must}, {nested, Occur::Should}}, /*min_should=*/2);
    ASSERT_TRUE(with_min.has_value());
    EXPECT_EQ(with_min.value().to_string("").value(), "(+a d)~2");
}

TEST(BooleanQueryTest, RewriteUnwrapsSingleClauseQueries) {
    auto searcher = make_searcher(2);
    auto must = fixed("a", {});
    auto query = boolean({{must, Occur::Must}});
    ASSERT_TRUE(query.has_value());

    auto rewritten = query.value().rewrite(*searcher);
    ASSERT_TRUE(rewritten.has_value());
    ASSERT_NE(rewritten.value(), nullptr);
    EXPECT_EQ(rewritten.value().get(), must.get());
}

TEST(BooleanQueryTest, DuplicateFilterClausesAreCollapsed) {
    auto must = fixed("a", {});
    auto filter = fixed("b", {});

    auto query = boolean({{must, Occur::Must}, {filter, Occur::Filter}, {fixed("b", {}), Occur::Filter}});
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(query.value().clauses().size(), 3U);
    // Both FILTER clauses are the same query, so only one of them is used.
    EXPECT_EQ(query.value().clause_indices(Occur::Filter).size(), 1U);
    EXPECT_EQ(query.value().clause_indices(Occur::Must).size(), 1U);
}

TEST(BooleanQueryTest, RejectsNegativeMinimumShouldMatch) {
    auto query = BooleanQuery::create({}, -1);
    ASSERT_FALSE(query.has_value());
    EXPECT_EQ(query.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(BooleanQueryTest, TheClauseLimitIsConfigurable) {
    // The default is the reference implementation's 1024.
    EXPECT_EQ(BooleanQuery::max_clause_count(), BooleanQuery::DEFAULT_MAX_CLAUSE_COUNT);
    EXPECT_EQ(IndexSearcher::get_max_clause_count(), BooleanQuery::DEFAULT_MAX_CLAUSE_COUNT);

    // A limit smaller than the clause count rejects the query, like Lucene's
    // `TooManyClauses`.
    auto lowered = IndexSearcher::set_max_clause_count(2);
    ASSERT_TRUE(lowered.has_value()) << (lowered ? "" : lowered.error().to_string());
    EXPECT_EQ(BooleanQuery::max_clause_count(), 2U);
    EXPECT_EQ(IndexSearcher::get_max_clause_count(), 2U);

    {
        // The limit is enforced when a clause is added, not when the query is
        // built (the reference implementation does the same).
        BooleanQuery::Builder builder;
        EXPECT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", "a")), Occur::Should).has_value());
        EXPECT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", "b")), Occur::Should).has_value());
        auto too_many = builder.add(std::make_shared<TermQuery>(Term::from_text("body", "c")), Occur::Should);
        ASSERT_FALSE(too_many.has_value());
        EXPECT_EQ(too_many.error().kind(), ErrorKind::kTooManyClauses);
        EXPECT_TRUE(builder.build().has_value());
    }

    // A query that fits the limit is still accepted.
    {
        BooleanQuery::Builder builder;
        for (const std::string_view text : {"a", "b"}) {
            EXPECT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", text)), Occur::Should)
                                .has_value());
        }
        EXPECT_TRUE(builder.build().has_value());
    }

    // Zero is not a legal limit.
    auto rejected = IndexSearcher::set_max_clause_count(0);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_EQ(BooleanQuery::max_clause_count(), 2U);

    // Leave the process-wide limit as it was for the other tests.
    ASSERT_TRUE(IndexSearcher::set_max_clause_count(BooleanQuery::DEFAULT_MAX_CLAUSE_COUNT).has_value());
    EXPECT_EQ(BooleanQuery::max_clause_count(), BooleanQuery::DEFAULT_MAX_CLAUSE_COUNT);
}
