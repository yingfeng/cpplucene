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
#include <expected>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.wand_scorer;

import lucene_core;

using namespace lucene;

namespace {

const std::vector<std::string> kTerms{"alpha", "beta", "gamma", "delta", "epsilon"};

FieldType body_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

FieldType id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

/// The terms document `index` holds: a deterministic subset of `kTerms`.
std::vector<std::string> terms_of(int32_t index) {
    std::vector<std::string> terms;
    for (size_t i = 0; i < kTerms.size(); ++i) {
        // A rotating pattern: every document has two to four terms, so plenty of
        // documents match two of them.
        if (((static_cast<uint32_t>(index) >> i) + static_cast<uint32_t>(i)) % 3U != 0U) {
            terms.push_back(kTerms[i]);
        }
    }
    return terms;
}

Document make_doc(int32_t index) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::to_string(index)}, id_type()));
    const std::vector<std::string> terms = terms_of(index);
    for (const std::string &term : terms) {
        doc.add(std::make_unique<Field>("body", FieldValue{term}, body_type()));
    }
    return doc;
}

std::shared_ptr<IndexSearcher> build_searcher(ByteBuffersDirectory &directory, WhitespaceAnalyzer &analyzer,
                                              int32_t doc_count) {
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    if (!writer.has_value()) {
        return nullptr;
    }
    for (int32_t i = 0; i < doc_count; ++i) {
        if (!writer.value()->add_document(make_doc(i)).has_value()) {
            return nullptr;
        }
    }
    if (!writer.value()->commit().has_value()) {
        return nullptr;
    }
    auto reader = DirectoryReader::open(directory);
    if (!reader.has_value()) {
        return nullptr;
    }
    return std::make_shared<IndexSearcher>(reader.value());
}

Result<BooleanQuery> make_query(int32_t min_should_match) {
    BooleanQuery::Builder builder;
    (void)builder.set_minimum_number_should_match(min_should_match);
    for (const std::string &term : kTerms) {
        if (auto added = builder.add(std::make_shared<TermQuery>(Term::from_text("body", term)), Occur::Should);
            !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return builder.build();
}

/// doc id -> score of a single-term query, for the whole index.
std::map<int32_t, float> scores_of(const IndexSearcher &searcher, const std::string &term) {
    std::map<int32_t, float> scores;
    auto top = searcher.search(TermQuery(Term::from_text("body", term)), 1000);
    if (!top.has_value()) {
        return scores;
    }
    for (const ScoreDoc &hit : top.value().score_docs) {
        scores[hit.doc] = hit.score;
    }
    return scores;
}

} // namespace

TEST(WandScorerTest, ScalingFactorFollowsTheMantissaRule) {
    // 1.0 has exponent 0, and the mantissa of a float has 24 bits.
    auto one = wand_scaling_factor(1.0F);
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one.value(), 23);

    auto two = wand_scaling_factor(2.0F);
    ASSERT_TRUE(two.has_value());
    EXPECT_EQ(two.value(), 22) << "a larger score needs fewer shifts";

    auto tenth = wand_scaling_factor(0.5F);
    ASSERT_TRUE(tenth.has_value());
    EXPECT_EQ(tenth.value(), 24) << "a smaller score needs more shifts";

    // The special cases are the ones that keep subnormals and infinity in range.
    auto zero = wand_scaling_factor(0.0F);
    ASSERT_TRUE(zero.has_value());
    auto min_value = wand_scaling_factor(std::numeric_limits<float>::denorm_min());
    ASSERT_TRUE(min_value.has_value());
    EXPECT_EQ(zero.value(), min_value.value() + 1);

    auto infinity = wand_scaling_factor(std::numeric_limits<float>::infinity());
    ASSERT_TRUE(infinity.has_value());
    auto max_value = wand_scaling_factor(std::numeric_limits<float>::max());
    ASSERT_TRUE(max_value.has_value());
    EXPECT_EQ(infinity.value(), max_value.value() - 1);

    EXPECT_FALSE(wand_scaling_factor(-1.0F).has_value()) << "scores are never negative";
}

TEST(WandScorerTest, ScaledScoresRoundTheSafeWay) {
    // Maximum scores round up (never miss a competitive match) ...
    EXPECT_EQ(wand_scale_max_score(1.0F, 23), 1 << 23);
    EXPECT_EQ(wand_scale_max_score(1.0000001F, 23), (1 << 23) + 1) << "rounded up";
    // ... minimum competitive scores round down (never prune a match away).
    EXPECT_EQ(wand_scale_min_score(1.0F, 23), 1 << 23);
    EXPECT_EQ(wand_scale_min_score(0.9999999F, 23), (1 << 23) - 1) << "rounded down";
    // A scorer that answers infinity is capped instead of overflowing.
    EXPECT_EQ(wand_scale_max_score(std::numeric_limits<float>::infinity(), 23), MAX_SCALED_SCORE);
    EXPECT_EQ(wand_scale_max_score(1.0F, 0), 1);
}

TEST(WandScorerTest, MinimumShouldMatchAgreesWithBruteForce) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    const int32_t doc_count = 200;
    auto searcher = build_searcher(directory, analyzer, doc_count);
    ASSERT_NE(searcher, nullptr);

    // At least two of the five terms have to match.
    auto query = make_query(2);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());

    // The same query without the constraint gives the score of each document.
    auto disjunction = make_query(1);
    ASSERT_TRUE(disjunction.has_value());
    auto all_hits = searcher->search(disjunction.value(), doc_count);
    ASSERT_TRUE(all_hits.has_value());
    std::map<int32_t, float> disjunction_scores;
    for (const ScoreDoc &hit : all_hits.value().score_docs) {
        disjunction_scores[hit.doc] = hit.score;
    }

    auto top = searcher->search(query.value(), doc_count);
    ASSERT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());

    // Brute force: every document with at least two of the terms.
    std::set<int32_t> expected;
    for (int32_t i = 0; i < doc_count; ++i) {
        const size_t matches = terms_of(i).size();
        if (matches >= 2) {
            expected.insert(i);
        }
    }
    EXPECT_EQ(top.value().score_docs.size(), expected.size());
    EXPECT_EQ(searcher->count(query.value()).value(), static_cast<int64_t>(expected.size()))
            << "count uses the exhaustive path";

    std::set<int32_t> actual;
    for (const ScoreDoc &hit : top.value().score_docs) {
        actual.insert(hit.doc);
        // A document that matches more than the required clauses is scored with all
        // of them, exactly like the plain disjunction.
        ASSERT_TRUE(disjunction_scores.contains(hit.doc));
        EXPECT_FLOAT_EQ(hit.score, disjunction_scores[hit.doc]) << "doc " << hit.doc;
    }
    EXPECT_EQ(actual, expected);

    // The scores are ordered, which is what a top-n search promises.
    for (size_t i = 1; i < top.value().score_docs.size(); ++i) {
        EXPECT_GE(top.value().score_docs[i - 1].score, top.value().score_docs[i].score) << "doc index " << i;
    }
}

TEST(WandScorerTest, TopScoresPruningKeepsTheSameTopHits) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    const int32_t doc_count = 300;
    auto searcher = build_searcher(directory, analyzer, doc_count);
    ASSERT_NE(searcher, nullptr);

    // A pure disjunction (minimum should match 1) goes through WAND as well when the
    // clause is the top-level scoring clause; the answer has to be the same as the
    // one of the exhaustive scorer.
    auto query = make_query(1);
    ASSERT_TRUE(query.has_value());

    auto small = searcher->search(query.value(), 5);
    ASSERT_TRUE(small.has_value()) << (small ? "" : small.error().to_string());
    auto large = searcher->search(query.value(), doc_count);
    ASSERT_TRUE(large.has_value());

    ASSERT_EQ(small.value().score_docs.size(), 5U);
    for (size_t i = 0; i < small.value().score_docs.size(); ++i) {
        EXPECT_EQ(small.value().score_docs[i].doc, large.value().score_docs[i].doc) << "position " << i;
        EXPECT_FLOAT_EQ(small.value().score_docs[i].score, large.value().score_docs[i].score) << "position " << i;
    }

    // And the score of a hit is the sum of the scores of its terms.
    auto top = searcher->search(query.value(), doc_count);
    ASSERT_TRUE(top.has_value());
    std::vector<std::map<int32_t, float>> per_term;
    per_term.reserve(kTerms.size());
    for (const std::string &term : kTerms) {
        per_term.push_back(scores_of(*searcher, term));
    }
    for (const ScoreDoc &hit : top.value().score_docs) {
        double sum = 0;
        for (const std::map<int32_t, float> &scores : per_term) {
            if (const auto found = scores.find(hit.doc); found != scores.end()) {
                sum += static_cast<double>(found->second);
            }
        }
        EXPECT_NEAR(hit.score, sum, 0.01) << "doc " << hit.doc;
    }
}

namespace {

/// The scorer of `term` over the only leaf of `searcher`.
std::unique_ptr<Scorer> scorer_of(const IndexSearcher &searcher, const std::string &term, ScoreMode score_mode) {
    const TermQuery query(Term::from_text("body", term));
    auto weight = query.create_weight(searcher, score_mode, 1.0F);
    if (!weight.has_value()) {
        return nullptr;
    }
    auto supplier = weight.value()->scorer_supplier(searcher.leaves().value().front(), searcher);
    if (!supplier.has_value() || supplier.value() == nullptr) {
        return nullptr;
    }
    auto scorer = supplier.value()->get(1, searcher.leaves().value().front(), searcher);
    if (!scorer.has_value()) {
        return nullptr;
    }
    return std::move(scorer.value());
}

} // namespace

TEST(WandScorerTest, RejectsAnImpossibleConfiguration) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto searcher = build_searcher(directory, analyzer, 10);
    ASSERT_NE(searcher, nullptr);

    {
        std::vector<std::unique_ptr<Scorer>> one;
        one.push_back(scorer_of(*searcher, kTerms[0], ScoreMode::Complete));
        ASSERT_NE(one.front(), nullptr);
        auto wand = WandScorer::create(std::move(one), 1, ScoreMode::TopScores, 1);
        EXPECT_FALSE(wand.has_value()) << "a disjunction needs at least two clauses";
        if (!wand.has_value()) {
            EXPECT_NE(wand.error().to_string().find("two sub-scorers"), std::string::npos);
        }
    }
    {
        std::vector<std::unique_ptr<Scorer>> two;
        two.push_back(scorer_of(*searcher, kTerms[0], ScoreMode::Complete));
        two.push_back(scorer_of(*searcher, kTerms[1], ScoreMode::Complete));
        ASSERT_NE(two[0], nullptr);
        ASSERT_NE(two[1], nullptr);
        auto wand = WandScorer::create(std::move(two), 3, ScoreMode::TopScores, 1);
        EXPECT_FALSE(wand.has_value()) << "min_should_match must not exceed the clause count";
        if (!wand.has_value()) {
            EXPECT_NE(wand.error().to_string().find("min_should_match"), std::string::npos);
        }
    }
}
