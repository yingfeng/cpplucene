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
#include <map>
#include <string>
#include <memory>
#include <vector>

module lucene_core:ut.boolean_scorer_window;

import lucene_core;

using namespace lucene;

namespace {

/// A scorer over a fixed, sorted list of documents with a constant score.
///
/// It is the oracle's building block: a brute force scan over the same lists
/// says exactly which documents match and with which score.
class ListScorer final : public Scorer {
public:
    ListScorer(std::vector<int32_t> docs, float score) : docs_(std::move(docs)), score_(score) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (i_ < docs_.size() && docs_[i_] < target) {
            ++i_;
        }
        doc_ = i_ < docs_.size() ? docs_[i_] : NO_MORE_DOCS;
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }
    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_; }
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override { return score_; }
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }
    [[nodiscard]] Result<float> score() override { return score_; }

private:
    std::vector<int32_t> docs_;
    float score_;
    size_t i_{0};
    int32_t doc_{-1};
};

/// Collects `(doc, score)` pairs.
class RecordingCollector final : public LeafCollector {
public:
    [[nodiscard]] VoidResult set_scorer(Scorable &scorer) override {
        scorer_ = &scorer;
        return {};
    }

    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &scorer) override {
        auto value = scorer.score();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        hits.emplace_back(doc, value.value());
        return {};
    }

    [[nodiscard]] std::string to_string() const override { return "RecordingCollector"; }

    std::vector<std::pair<int32_t, float>> hits;

private:
    Scorable *scorer_{nullptr};
};

/// The brute force answer: document -> (frequency, score sum).
std::map<int32_t, std::pair<size_t, float>> oracle(const std::vector<std::pair<std::vector<int32_t>, float>> &clauses,
                                                   size_t min_should_match) {
    std::map<int32_t, std::pair<size_t, float>> expected;
    for (const auto &[docs, score] : clauses) {
        for (const int32_t doc : docs) {
            auto &[freq, sum] = expected[doc];
            ++freq;
            sum += score;
        }
    }
    for (auto entry = expected.begin(); entry != expected.end();) {
        if (entry->second.first < min_should_match) {
            entry = expected.erase(entry);
        } else {
            ++entry;
        }
    }
    return expected;
}

std::vector<std::unique_ptr<Scorer>> scorers_of(const std::vector<std::pair<std::vector<int32_t>, float>> &clauses) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    scorers.reserve(clauses.size());
    for (const auto &[docs, score] : clauses) {
        scorers.push_back(std::make_unique<ListScorer>(docs, score));
    }
    return scorers;
}

void expect_matches(const std::vector<std::pair<std::vector<int32_t>, float>> &clauses, size_t min_should_match,
                    bool needs_scores, int32_t max_doc) {
    auto expected = oracle(clauses, min_should_match);
    auto scorer = BooleanScorer::create(scorers_of(clauses), min_should_match, needs_scores);
    ASSERT_TRUE(scorer.has_value()) << (scorer ? "" : scorer.error().to_string());

    RecordingCollector collector;
    auto last = scorer.value()->score(collector, nullptr, 0, max_doc);
    ASSERT_TRUE(last.has_value()) << (last ? "" : last.error().to_string());

    ASSERT_EQ(collector.hits.size(), expected.size());
    size_t index = 0;
    for (const auto &[doc, pair] : expected) {
        EXPECT_EQ(collector.hits[index].first, doc);
        if (needs_scores) {
            EXPECT_NEAR(collector.hits[index].second, pair.second, 1e-5F) << "doc " << doc;
        }
        ++index;
    }
}

} // namespace

TEST(BooleanScorerWindowTest, ScoresADisjunctionWithScores) {
    expect_matches({{{{1, 5, 9, 100}}, 1.5F}, {{{5, 7, 100}}, 2.5F}}, 1, true, 200);
}

TEST(BooleanScorerWindowTest, CountsWithoutScores) {
    expect_matches({{{{2, 4, 6}}, 1.0F}, {{{4, 8}}, 1.0F}, {{{6, 8, 10}}, 1.0F}}, 1, false, 200);
}

TEST(BooleanScorerWindowTest, AppliesTheMinimumShouldMatch) {
    // Two of the three clauses have to match.
    expect_matches({{{{1, 2, 3}}, 1.0F}, {{{2, 3, 4}}, 2.0F}, {{{3, 4, 5}}, 3.0F}}, 2, true, 200);
    // Every clause has to match.
    expect_matches({{{{1, 2, 3}}, 1.0F}, {{{2, 3, 4}}, 2.0F}, {{{3, 15}}, 3.0F}}, 3, true, 200);
}

TEST(BooleanScorerWindowTest, WalksAcrossWindowBoundaries) {
    // Documents on both sides of the 4096 boundary, and one clause that only
    // matches in the second window.
    const std::vector<int32_t> first{0, 1, 4095, 4096, 4097, 8191};
    const std::vector<int32_t> second{4095, 4096, 8191, 8192};
    expect_matches({{first, 1.0F}, {second, 2.0F}}, 1, true, 8193);
    expect_matches({{first, 1.0F}, {second, 2.0F}}, 2, true, 8193);
}

TEST(BooleanScorerWindowTest, RespectsTheAcceptedDocuments) {
    auto clauses = std::vector<std::pair<std::vector<int32_t>, float>>{{{{1, 2, 3, 4}}, 1.0F}, {{{3, 4, 5}}, 1.0F}};
    auto scorer = BooleanScorer::create(scorers_of(clauses), 1, true);
    ASSERT_TRUE(scorer.has_value());

    // Only the even documents may match.
    FixedBitSet accept(10);
    for (const int32_t doc : {1, 2, 3, 4, 5}) {
        if (doc % 2 == 0) {
            accept.set(static_cast<size_t>(doc));
        }
    }
    RecordingCollector collector;
    ASSERT_TRUE(scorer.value()->score(collector, &accept, 0, 10).has_value());
    ASSERT_EQ(collector.hits.size(), 2U);
    EXPECT_EQ(collector.hits[0].first, 2);
    EXPECT_EQ(collector.hits[1].first, 4);
}

TEST(BooleanScorerWindowTest, RejectsFewerThanTwoClauses) {
    std::vector<std::unique_ptr<Scorer>> one;
    one.push_back(std::make_unique<ListScorer>(std::vector<int32_t>{1}, 1.0F));
    auto scorer = BooleanScorer::create(std::move(one), 1, true);
    ASSERT_FALSE(scorer.has_value());
    EXPECT_EQ(scorer.error().kind(), ErrorKind::kIllegalArgument);

    // A minimum should match above the clause count is rejected as well.
    const std::vector<std::pair<std::vector<int32_t>, float>> two_clauses{{{1}, 1.0F}, {{2}, 1.0F}};
    auto too_many = BooleanScorer::create(scorers_of(two_clauses), 3, true);
    ASSERT_FALSE(too_many.has_value());
    EXPECT_EQ(too_many.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(BooleanScorerWindowTest, ReportsItsCostAndConfiguration) {
    auto scorer = BooleanScorer::create(scorers_of({{{{1, 2, 3}}, 1.0F}, {{{2, 3, 4}}, 1.0F}}), 2, true);
    ASSERT_TRUE(scorer.has_value());
    EXPECT_EQ(scorer.value()->min_should_match(), 2U);
    EXPECT_TRUE(scorer.value()->needs_scores());
    EXPECT_EQ(scorer.value()->num_scorers(), 2U);
    EXPECT_GT(scorer.value()->cost().value(), 0);
}

namespace {

FieldType keyword_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

/// Six documents with the terms of the disjunction in every combination.
std::shared_ptr<IndexSearcher> disjunction_searcher(ByteBuffersDirectory &directory, WhitespaceAnalyzer &analyzer) {
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    EXPECT_TRUE(writer.has_value());
    if (!writer.has_value()) {
        return nullptr;
    }
    const std::vector<std::vector<std::string>> docs{{"a"}, {"b"}, {"c"}, {"a", "b"}, {"a", "b", "c"}, {"d"}};
    for (const std::vector<std::string> &terms : docs) {
        Document doc;
        for (const std::string &term : terms) {
            doc.add(std::make_unique<Field>(std::string("body"), FieldValue{term}, keyword_type()));
        }
        EXPECT_TRUE(writer.value()->add_document(doc).has_value());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    EXPECT_TRUE(writer.value()->close().has_value());
    auto reader = DirectoryReader::open(directory);
    EXPECT_TRUE(reader.has_value());
    if (!reader.has_value()) {
        return nullptr;
    }
    return std::make_shared<IndexSearcher>(reader.value());
}

Term term_of(std::string_view text) { return Term::from_text("body", text); }

} // namespace

TEST(BooleanScorerWindowTest, CountsARealDisjunctionThroughTheWindowScorer) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto searcher = disjunction_searcher(directory, analyzer);
    ASSERT_NE(searcher, nullptr);

    BooleanQuery::Builder builder;
    for (const std::string_view text : {"a", "b", "c"}) {
        EXPECT_TRUE(builder.add(std::make_shared<TermQuery>(term_of(text)), Occur::Should).has_value());
    }
    auto query = builder.build();
    ASSERT_TRUE(query.has_value());

    // `count` collects no scores, so the disjunction goes through the windowed
    // scorer: documents with any of a/b/c, i.e. five of the six.
    EXPECT_EQ(searcher->count(query.value()).value(), 5);
}

TEST(BooleanScorerWindowTest, KeepsFiltersAndMinimumShouldMatchCorrect) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto searcher = disjunction_searcher(directory, analyzer);
    ASSERT_NE(searcher, nullptr);

    // A FILTER clause keeps the disjunction on the max-score path.
    BooleanQuery::Builder filtered;
    EXPECT_TRUE(filtered.add(std::make_shared<TermQuery>(term_of("a")), Occur::Should).has_value());
    EXPECT_TRUE(filtered.add(std::make_shared<TermQuery>(term_of("b")), Occur::Should).has_value());
    EXPECT_TRUE(filtered.add(std::make_shared<TermQuery>(term_of("c")), Occur::Should).has_value());
    EXPECT_TRUE(filtered.add(std::make_shared<TermQuery>(term_of("d")), Occur::Filter).has_value());
    auto filtered_query = filtered.build();
    ASSERT_TRUE(filtered_query.has_value());
    EXPECT_EQ(searcher->count(filtered_query.value()).value(), 1);

    // At least two of a/b/c have to match: documents 4 and 5.
    BooleanQuery::Builder minimum;
    (void)minimum.set_minimum_number_should_match(2);
    for (const std::string_view text : {"a", "b", "c"}) {
        EXPECT_TRUE(minimum.add(std::make_shared<TermQuery>(term_of(text)), Occur::Should).has_value());
    }
    auto minimum_query = minimum.build();
    ASSERT_TRUE(minimum_query.has_value());
    EXPECT_EQ(minimum_query.value().minimum_number_should_match(), 2);
    auto top = searcher->search(minimum_query.value(), 10);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top.value().score_docs.size(), 2U) << "search honours minimum_should_match";
    EXPECT_EQ(searcher->count(minimum_query.value()).value(), 2) << "count honours minimum_should_match";
}

TEST(BooleanScorerWindowTest, ConstantScoreKeepsTheConfirmationStep) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto searcher = disjunction_searcher(directory, analyzer);
    ASSERT_NE(searcher, nullptr);

    // A constant score wrapper must not drop the confirmation step of the query
    // it wraps: the two-phase iterator of `minimum_should_match` yields the union
    // and only `matches()` narrows it down.
    BooleanQuery::Builder inner;
    (void)inner.set_minimum_number_should_match(2);
    for (const std::string_view text : {"a", "b", "c"}) {
        EXPECT_TRUE(inner.add(std::make_shared<TermQuery>(term_of(text)), Occur::Should).has_value());
    }
    auto inner_query = inner.build();
    ASSERT_TRUE(inner_query.has_value());

    auto constant = std::make_shared<ConstantScoreQuery>(std::make_shared<BooleanQuery>(inner_query.value()));
    EXPECT_EQ(searcher->count(*constant).value(), 2);

    auto top = searcher->search(*constant, 10);
    ASSERT_TRUE(top.has_value());
    ASSERT_EQ(top.value().score_docs.size(), 2U);
    // Every hit has the constant score.
    for (const ScoreDoc &hit : top.value().score_docs) {
        EXPECT_FLOAT_EQ(hit.score, 1.0F);
    }
}
