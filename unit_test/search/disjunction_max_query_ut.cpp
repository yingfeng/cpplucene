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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.disjunction_max_query;

import lucene_core;

using namespace lucene;

namespace {

/// One document and the score the fake query gives it.
struct Hit {
    int32_t doc{0};
    float score{0.0F};
};

/// A scorer that walks a fixed list of documents and scores.
class ScriptedScorer final : public Scorer {
public:
    explicit ScriptedScorer(std::vector<Hit> hits) : hits_(std::move(hits)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++index_;
        return set_doc();
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_ != NO_MORE_DOCS && doc_ < target) {
            if (auto next = next_doc(); !next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(hits_.size()); }
    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_; }
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override { return maximum_; }
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }

    [[nodiscard]] Result<float> score() override {
        if (index_ < 0 || index_ >= static_cast<int32_t>(hits_.size())) {
            return err(LuceneError::illegal_state("the scorer is not positioned"));
        }
        return hits_[static_cast<size_t>(index_)].score;
    }

private:
    [[nodiscard]] int32_t set_doc() {
        if (index_ >= static_cast<int32_t>(hits_.size())) {
            doc_ = NO_MORE_DOCS;
        } else {
            doc_ = hits_[static_cast<size_t>(index_)].doc;
        }
        return doc_;
    }

    std::vector<Hit> hits_;
    int32_t index_{-1};
    int32_t doc_{-1};
    float maximum_{100.0F};
};

/// A supplier that hands out one scripted scorer.
class ScriptedScorerSupplier final : public ScorerSupplier {
public:
    explicit ScriptedScorerSupplier(std::vector<Hit> hits) : hits_(std::move(hits)) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        return std::unique_ptr<Scorer>(new ScriptedScorer(hits_));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return static_cast<int64_t>(hits_.size());
    }

private:
    std::vector<Hit> hits_;
};

/// A weight over a scripted scorer; it also produces explanations, which is all
/// [`DisjunctionMaxQuery`]'s explain needs from its disjuncts.
class ScriptedWeight final : public Weight {
public:
    ScriptedWeight(const Query &query, std::vector<Hit> hits) : query_(&query), hits_(std::move(hits)) {}

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext & /*context*/, int32_t doc,
                                              const IndexSearcher & /*searcher*/) const override {
        for (const Hit &hit : hits_) {
            if (hit.doc == doc) {
                return Explanation::match_no_details(hit.score, "scripted");
            }
        }
        return Explanation::no_match_no_details("scripted");
    }

    [[nodiscard]] const Query &get_query() const override { return *query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext & /*context*/,
                                                                         const IndexSearcher & /*searcher*/) override {
        return std::unique_ptr<ScorerSupplier>(new ScriptedScorerSupplier(hits_));
    }

private:
    const Query *query_;
    std::vector<Hit> hits_;
};

/// A query whose weight scores the documents it was created with.
class ScriptedQuery final : public Query {
public:
    explicit ScriptedQuery(std::vector<Hit> hits, std::string name = "scripted")
            : hits_(std::move(hits)), name_(std::move(name)) {}

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher & /*searcher*/,
                                                                ScoreMode /*score_mode*/,
                                                                float /*boost*/) const override {
        std::unique_ptr<Weight> weight(new ScriptedWeight(*this, hits_));
        return weight;
    }

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override {
        if (!visitor.accept_field("scripted")) {
            return {};
        }
        return visitor.visit_leaf(*this);
    }

    [[nodiscard]] Result<std::string> to_string(std::string_view /*field*/) const override { return name_; }

    [[nodiscard]] bool equals(const Query &other) const override {
        const auto *scripted = dynamic_cast<const ScriptedQuery *>(&other);
        if (scripted == nullptr) {
            return false;
        }
        if (name_ != scripted->name_ || hits_.size() != scripted->hits_.size()) {
            return false;
        }
        for (size_t i = 0; i < hits_.size(); ++i) {
            if (hits_[i].doc != scripted->hits_[i].doc || hits_[i].score != scripted->hits_[i].score) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] size_t hash() const override { return std::hash<std::string>{}(name_); }

private:
    std::vector<Hit> hits_;
    std::string name_;
};

/// An [`IndexReader`] without leaves; the scripted queries never touch a reader.
class EmptyIndexReader final : public IndexReader {
public:
    [[nodiscard]] Result<int32_t> max_doc() const override { return 0; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return 0; }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }
    [[nodiscard]] Result<int32_t> doc_freq(const Term & /*term*/) override { return 0; }
    [[nodiscard]] Result<int64_t> total_term_freq(const Term & /*term*/) override { return 0; }
    [[nodiscard]] Result<int64_t> sum_doc_freq(std::string_view /*field*/) override { return 0; }
    [[nodiscard]] Result<int32_t> doc_count(std::string_view /*field*/) override { return 0; }
    [[nodiscard]] Result<int64_t> sum_total_term_freq(std::string_view /*field*/) override { return 0; }
    [[nodiscard]] Result<std::span<const LeafReaderContext>> leaves() const override {
        return std::span<const LeafReaderContext>(empty_);
    }
    [[nodiscard]] const Identity &identity() const override { return identity_; }

private:
    Identity identity_{};
    std::vector<LeafReaderContext> empty_{};
};

/// A searcher without leaves; the scripted queries never touch a reader.
IndexSearcher make_searcher() { return IndexSearcher(std::make_shared<EmptyIndexReader>()); }

/// Drains `query` and returns the documents with their scores.
std::vector<Hit> collect(IndexSearcher &searcher, const Query &query, float boost = 1.0F) {
    std::vector<Hit> hits;
    auto weight = query.create_weight(searcher, ScoreMode::TopScores, boost);
    EXPECT_TRUE(weight.has_value()) << (weight.has_value() ? "" : weight.error().to_string());
    if (!weight.has_value()) {
        return hits;
    }
    LeafReaderContext context(nullptr, 0, 0, TopParentMeta{1, 0, Identity{}});
    auto scorer = weight.value()->scorer(context, searcher);
    EXPECT_TRUE(scorer.has_value()) << (scorer.has_value() ? "" : scorer.error().to_string());
    if (!scorer.has_value() || scorer.value() == nullptr) {
        return hits;
    }
    while (true) {
        auto doc = scorer.value()->next_doc();
        EXPECT_TRUE(doc.has_value()) << (doc.has_value() ? "" : doc.error().to_string());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto score = scorer.value()->score();
        EXPECT_TRUE(score.has_value()) << (score.has_value() ? "" : score.error().to_string());
        hits.push_back({doc.value(), score.has_value() ? score.value() : 0.0F});
    }
    return hits;
}

} // namespace

TEST(DisjunctionMaxQueryTest, ScoresMaxPlusTieBreakerTimesTheOthers) {
    // doc 0 matches both disjuncts, doc 1 only the first.
    auto first = std::make_shared<ScriptedQuery>(std::vector<Hit>{{0, 3.0F}, {1, 2.0F}});
    auto second = std::make_shared<ScriptedQuery>(std::vector<Hit>{{0, 1.0F}});
    auto query = DisjunctionMaxQuery::create({first, second}, 0.5F);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();

    IndexSearcher searcher = make_searcher();
    const auto hits = collect(searcher, *query.value());
    ASSERT_EQ(hits.size(), 2U);
    EXPECT_EQ(hits[0].doc, 0);
    EXPECT_FLOAT_EQ(hits[0].score, 3.5F); // 3 + 0.5 * 1
    EXPECT_EQ(hits[1].doc, 1);
    EXPECT_FLOAT_EQ(hits[1].score, 2.0F);
}

TEST(DisjunctionMaxQueryTest, ZeroTieBreakerTakesTheMaximumOnly) {
    auto first = std::make_shared<ScriptedQuery>(std::vector<Hit>{{0, 3.0F}});
    auto second = std::make_shared<ScriptedQuery>(std::vector<Hit>{{0, 1.0F}});
    auto query = DisjunctionMaxQuery::create({first, second}, 0.0F);
    ASSERT_TRUE(query.has_value());

    IndexSearcher searcher = make_searcher();
    const auto hits = collect(searcher, *query.value());
    ASSERT_EQ(hits.size(), 1U);
    EXPECT_FLOAT_EQ(hits[0].score, 3.0F);
}

TEST(DisjunctionMaxQueryTest, RejectsATieBreakerOutsideTheUnitInterval) {
    auto first = std::make_shared<ScriptedQuery>(std::vector<Hit>{});
    auto negative = DisjunctionMaxQuery::create({first}, -0.1F);
    EXPECT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().kind(), ErrorKind::kIllegalArgument);
    auto too_big = DisjunctionMaxQuery::create({first}, 1.1F);
    EXPECT_FALSE(too_big.has_value());
    auto one = DisjunctionMaxQuery::create({first}, 1.0F);
    EXPECT_TRUE(one.has_value());
}

TEST(DisjunctionMaxQueryTest, RejectsNullDisjuncts) {
    std::shared_ptr<Query> null_query;
    auto query = DisjunctionMaxQuery::create({null_query}, 0.0F);
    EXPECT_FALSE(query.has_value());
    EXPECT_EQ(query.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(DisjunctionMaxQueryTest, RewritesAnEmptyQueryToMatchNoDocs) {
    auto query = DisjunctionMaxQuery::create({}, 0.0F);
    ASSERT_TRUE(query.has_value());
    IndexSearcher searcher = make_searcher();
    auto rewritten = query.value()->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    ASSERT_NE(rewritten.value(), nullptr);
    EXPECT_NE(dynamic_cast<const MatchNoDocsQuery *>(rewritten.value().get()), nullptr);
}

TEST(DisjunctionMaxQueryTest, RewritesASingleDisjunctToItself) {
    auto only = std::make_shared<ScriptedQuery>(std::vector<Hit>{{0, 1.0F}}, "only");
    auto query = DisjunctionMaxQuery::create({only}, 0.2F);
    ASSERT_TRUE(query.has_value());
    IndexSearcher searcher = make_searcher();
    auto rewritten = query.value()->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value());
    EXPECT_EQ(rewritten.value().get(), only.get());
}

TEST(DisjunctionMaxQueryTest, RewritesAFullTieBreakerToABooleanQuery) {
    auto first = std::make_shared<ScriptedQuery>(std::vector<Hit>{{0, 1.0F}}, "a");
    auto second = std::make_shared<ScriptedQuery>(std::vector<Hit>{{0, 2.0F}}, "b");
    auto query = DisjunctionMaxQuery::create({first, second}, 1.0F);
    ASSERT_TRUE(query.has_value());
    IndexSearcher searcher = make_searcher();
    auto rewritten = query.value()->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value());
    const auto *boolean = dynamic_cast<const BooleanQuery *>(rewritten.value().get());
    ASSERT_NE(boolean, nullptr);
    EXPECT_EQ(boolean->clauses().size(), 2U);
}

TEST(DisjunctionMaxQueryTest, DoesNotRewriteWhenNoDisjunctChanges) {
    auto first = std::make_shared<ScriptedQuery>(std::vector<Hit>{{0, 1.0F}}, "a");
    auto second = std::make_shared<ScriptedQuery>(std::vector<Hit>{{0, 2.0F}}, "b");
    auto query = DisjunctionMaxQuery::create({first, second}, 0.1F);
    ASSERT_TRUE(query.has_value());
    IndexSearcher searcher = make_searcher();
    auto rewritten = query.value()->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value());
    EXPECT_EQ(rewritten.value(), nullptr);
}

TEST(DisjunctionMaxQueryTest, TextFormShowsTheDisjunctsAndTheTieBreaker) {
    auto first = std::make_shared<ScriptedQuery>(std::vector<Hit>{}, "a");
    auto second = std::make_shared<ScriptedQuery>(std::vector<Hit>{}, "b");
    auto with_tie = DisjunctionMaxQuery::create({first, second}, 0.1F);
    ASSERT_TRUE(with_tie.has_value());
    EXPECT_EQ(with_tie.value()->to_string("").value(), "(a | b)~0.1");

    auto without_tie = DisjunctionMaxQuery::create({first, second}, 0.0F);
    ASSERT_TRUE(without_tie.has_value());
    EXPECT_EQ(without_tie.value()->to_string("").value(), "(a | b)");
}

TEST(DisjunctionMaxQueryTest, EqualityIgnoresTheDisjunctOrder) {
    auto first = std::make_shared<ScriptedQuery>(std::vector<Hit>{{0, 1.0F}}, "a");
    auto second = std::make_shared<ScriptedQuery>(std::vector<Hit>{{1, 2.0F}}, "b");
    auto forward = DisjunctionMaxQuery::create({first, second}, 0.3F);
    auto backward = DisjunctionMaxQuery::create({second, first}, 0.3F);
    auto other_tie = DisjunctionMaxQuery::create({first, second}, 0.4F);
    ASSERT_TRUE(forward.has_value());
    ASSERT_TRUE(backward.has_value());
    ASSERT_TRUE(other_tie.has_value());
    EXPECT_TRUE(forward.value()->equals(*backward.value()));
    EXPECT_EQ(forward.value()->hash(), backward.value()->hash());
    EXPECT_FALSE(forward.value()->equals(*other_tie.value()));
}

TEST(DisjunctionMaxQueryTest, ExplainDescribesTheMaximumAndTheOthers) {
    auto first = std::make_shared<ScriptedQuery>(std::vector<Hit>{{0, 3.0F}});
    auto second = std::make_shared<ScriptedQuery>(std::vector<Hit>{{0, 1.0F}});
    auto query = DisjunctionMaxQuery::create({first, second}, 0.5F);
    ASSERT_TRUE(query.has_value());

    IndexSearcher searcher = make_searcher();
    auto weight = query.value()->create_weight(searcher, ScoreMode::TopScores, 1.0F);
    ASSERT_TRUE(weight.has_value());
    LeafReaderContext context(nullptr, 0, 0, TopParentMeta{1, 0, Identity{}});
    auto explanation = weight.value()->explain(context, 0, searcher);
    ASSERT_TRUE(explanation.has_value()) << explanation.error().to_string();
    EXPECT_TRUE(explanation.value().is_match());
    EXPECT_FLOAT_EQ(static_cast<float>(number_as_f64(explanation.value().value())), 3.5F);
    EXPECT_EQ(explanation.value().details().size(), 2U);

    auto no_match = weight.value()->explain(context, 5, searcher);
    ASSERT_TRUE(no_match.has_value());
    EXPECT_FALSE(no_match.value().is_match());
}

TEST(DisjunctionMaxQueryTest, DisjunctionMaxScorerValidatesItsArguments) {
    auto scorer = std::make_unique<ScriptedScorer>(std::vector<Hit>{{0, 1.0F}});
    std::vector<std::unique_ptr<Scorer>> too_few;
    too_few.push_back(std::move(scorer));
    auto single = DisjunctionMaxScorer::create(0.0F, std::move(too_few), ScoreMode::TopScores);
    EXPECT_FALSE(single.has_value());
    EXPECT_EQ(single.error().kind(), ErrorKind::kIllegalArgument);

    std::vector<std::unique_ptr<Scorer>> pair;
    pair.push_back(std::make_unique<ScriptedScorer>(std::vector<Hit>{{0, 1.0F}}));
    pair.push_back(std::make_unique<ScriptedScorer>(std::vector<Hit>{{0, 2.0F}}));
    auto bad_tie = DisjunctionMaxScorer::create(2.0F, std::move(pair), ScoreMode::TopScores);
    EXPECT_FALSE(bad_tie.has_value());
    EXPECT_EQ(bad_tie.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(DisjunctionMaxQueryTest, TheBulkPathAgreesWithTheCountAndTheFullSearch) {
    // A plain maximum in `TopScores` mode is scored a block at a time, so the
    // block path has to agree with the count and with the full search.
    ByteBuffersDirectory directory;
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        config.set_max_buffered_docs(7); // several segments
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
        FieldType type;
        ASSERT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
        for (int32_t doc = 0; doc < 40; ++doc) {
            Document document;
            document.add(std::make_unique<Field>("alpha", FieldValue{std::string("alpha")}, type));
            if (doc % 3 == 0) {
                document.add(std::make_unique<Field>("beta", FieldValue{std::string("beta")}, type));
            }
            ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
        }
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    const IndexSearcher searcher(reader.value());

    std::vector<std::shared_ptr<Query>> disjuncts{std::make_shared<TermQuery>(Term::from_text("alpha", "alpha")),
                                                  std::make_shared<TermQuery>(Term::from_text("beta", "beta"))};
    auto query = DisjunctionMaxQuery::create(std::move(disjuncts), 0.0F);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();

    auto counted = searcher.count(*query.value());
    ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
    EXPECT_EQ(counted.value(), 40); // every document holds "alpha"

    auto top_five = searcher.search(*query.value(), 5);
    ASSERT_TRUE(top_five.has_value()) << top_five.error().to_string();
    auto all = searcher.search(*query.value(), 40);
    ASSERT_TRUE(all.has_value()) << all.error().to_string();

    EXPECT_EQ(top_five.value().total_hits.value, counted.value());
    EXPECT_EQ(all.value().total_hits.value, counted.value());
    ASSERT_EQ(top_five.value().score_docs.size(), 5U);
    ASSERT_EQ(all.value().score_docs.size(), 40U);
    // The first page is the head of the full result, scores included.
    for (size_t i = 0; i < top_five.value().score_docs.size(); ++i) {
        EXPECT_EQ(top_five.value().score_docs[i].doc, all.value().score_docs[i].doc) << i;
        EXPECT_FLOAT_EQ(top_five.value().score_docs[i].score, all.value().score_docs[i].score) << i;
    }

    // A document holding both terms scores higher than one holding only "alpha".
    auto with_both = searcher.search(*query.value(), 40);
    ASSERT_TRUE(with_both.has_value());
    EXPECT_GT(with_both.value().score_docs.front().score, with_both.value().score_docs.back().score);
}
