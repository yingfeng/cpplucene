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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.tf_idf_similarity;

import lucene_core;

using namespace lucene;

namespace {

/// 100 documents, field length 10 on average.
CollectionStatistics collection(std::string field = "f") {
    auto stats = CollectionStatistics::create(std::move(field), /*max_doc=*/100, /*doc_count=*/100,
                                              /*sum_total_term_freq=*/1000, /*sum_doc_freq=*/500);
    EXPECT_TRUE(stats.has_value());
    return stats.value();
}

TermStatistics term_stats(int64_t doc_freq = 10, int64_t total_term_freq = 20, std::string text = "apple") {
    auto stats = TermStatistics::create(Term::from_text("f", std::move(text)), doc_freq, total_term_freq);
    EXPECT_TRUE(stats.has_value());
    return stats.value();
}

/// One term with the default statistics, as a span.
std::span<const TermStatistics> single_term() {
    static const std::vector<TermStatistics> one{term_stats()};
    return one;
}

float near_float(double expected) {
    return static_cast<float>(expected);
}

/// The similarity a per-field wrapper serves, chosen by field name.
class FieldDispatchSimilarity final : public PerFieldSimilarityWrapper {
public:
    FieldDispatchSimilarity() : bm25_(BM25Similarity::default_similarity().value()) {}

    [[nodiscard]] const Similarity &get(std::string_view field) const override {
        if (field == "raw") {
            return raw_;
        }
        return bm25_;
    }

    [[nodiscard]] std::string name() const override { return "FieldDispatch"; }

private:
    BM25Similarity bm25_;
    RawTFSimilarity raw_{};
};

} // namespace

TEST(ClassicSimilarityTest, ComputesTheHistoricalFactors) {
    const ClassicSimilarity classic;

    EXPECT_FLOAT_EQ(classic.tf(4.0F), 2.0F);
    EXPECT_FLOAT_EQ(classic.length_norm(4), 0.5F);
    // log((doc_count + 1) / (doc_freq + 1)) + 1
    EXPECT_FLOAT_EQ(classic.idf(9, 99), near_float(std::log(10.0) + 1.0));
    EXPECT_FLOAT_EQ(classic.idf(0, 0), 1.0F);
    EXPECT_EQ(classic.name(), "ClassicSimilarity");
}

TEST(ClassicSimilarityTest, ExplainsTheIdf) {
    const ClassicSimilarity classic;
    auto explained = classic.idf_explain(collection(), term_stats());
    EXPECT_TRUE(explained.is_match());
    EXPECT_NEAR(number_as_f64(explained.value()), classic.idf(10, 100), 1e-4);
    EXPECT_NE(explained.description().find("log((docCount+1)/(docFreq+1))"), std::string::npos);
}

TEST(TFIDFSimilarityTest, DefaultsToClassicFactors) {
    const TFIDFSimilarity similarity = classic_similarity();
    EXPECT_EQ(similarity.name(), "ClassicSimilarity");
    EXPECT_TRUE(similarity.get_discount_overlaps());
    EXPECT_FALSE(classic_similarity_with_discount_overlaps(false).get_discount_overlaps());
}

TEST(TFIDFSimilarityTest, ScoresThePracticalScoringFunction) {
    const TFIDFSimilarity similarity = classic_similarity();
    auto scorer = similarity.scorer(/*boost=*/2.0F, collection(), single_term());
    ASSERT_TRUE(scorer.has_value()) << (scorer ? "" : scorer.error().to_string());

    const float idf = static_cast<float>(std::log(101.0 / 11.0) + 1.0);
    const auto &lengths = TFIDFScorer::length_table();
    // Encoded norm 5 decodes to a field length of 5 through the byte4 table.
    const float field_length = static_cast<float>(lengths[5]);
    const double expected = std::sqrt(4.0) * 2.0 * idf * (1.0 / std::sqrt(field_length));
    EXPECT_NEAR(scorer.value()->score(4.0F, 5), static_cast<float>(expected), 1e-4);

    // Monotonic in the term frequency.
    EXPECT_LT(scorer.value()->score(1.0F, 5), scorer.value()->score(4.0F, 5));
    // And non-increasing in the field length.
    EXPECT_GE(scorer.value()->score(4.0F, 5), scorer.value()->score(4.0F, 40));

    const auto *tf_idf_scorer = dynamic_cast<const TFIDFScorer *>(scorer.value().get());
    ASSERT_NE(tf_idf_scorer, nullptr);
    EXPECT_FLOAT_EQ(tf_idf_scorer->query_weight(), 2.0F * idf);
    // An empty field (norm 0) scores highest.
    EXPECT_GT(tf_idf_scorer->norm_table()[0], tf_idf_scorer->norm_table()[255]);
}

TEST(TFIDFSimilarityTest, ExplainsEveryFactor) {
    const TFIDFSimilarity similarity = classic_similarity();
    auto scorer = similarity.scorer(/*boost=*/2.0F, collection(), single_term());
    ASSERT_TRUE(scorer.has_value());

    auto explained = scorer.value()->explain(Explanation::match_no_details(4.0F, "freq"), 5);
    ASSERT_TRUE(explained.has_value()) << (explained ? "" : explained.error().to_string());
    const std::string text = explained.value().to_string();
    EXPECT_NE(text.find("boost"), std::string::npos);
    EXPECT_NE(text.find("idf"), std::string::npos);
    EXPECT_NE(text.find("tf(freq=4)"), std::string::npos);
    EXPECT_NE(text.find("fieldNorm"), std::string::npos);
}

TEST(TFIDFSimilarityTest, SumsTheIdfOfAPhrase) {
    const TFIDFSimilarity similarity = classic_similarity();
    const std::vector<TermStatistics> two{term_stats(10, 20, "apple"), term_stats(20, 40, "pie")};
    auto scorer = similarity.scorer(/*boost=*/1.0F, collection(), two);
    ASSERT_TRUE(scorer.has_value()) << (scorer ? "" : scorer.error().to_string());

    const auto *tf_idf_scorer = dynamic_cast<const TFIDFScorer *>(scorer.value().get());
    ASSERT_NE(tf_idf_scorer, nullptr);
    const double expected = (std::log(101.0 / 11.0) + 1.0) + (std::log(101.0 / 21.0) + 1.0);
    EXPECT_NEAR(number_as_f64(tf_idf_scorer->idf().value()), expected, 1e-4);
}

TEST(RawTFSimilarityTest, ScoresTheRawFrequency) {
    const RawTFSimilarity similarity;
    EXPECT_EQ(similarity.name(), "RawTFSimilarity");
    EXPECT_TRUE(similarity.get_discount_overlaps());

    auto scorer = similarity.scorer(3.0F, collection(), {});
    ASSERT_TRUE(scorer.has_value());
    EXPECT_FLOAT_EQ(scorer.value()->score(7.0F, 5), 21.0F);

    // The default explanation is the plain value.
    auto explained = scorer.value()->explain(Explanation::match_no_details(7.0F, "freq"), 5);
    ASSERT_TRUE(explained.has_value());
    EXPECT_FLOAT_EQ(static_cast<float>(number_as_f64(explained.value().value())), 21.0F);
}

TEST(PerFieldSimilarityWrapperTest, DispatchesByField) {
    const FieldDispatchSimilarity similarity;
    EXPECT_FALSE(similarity.get("f").name().empty());
    EXPECT_EQ(similarity.get("raw").name(), "RawTFSimilarity");

    FieldInvertState state(6, "raw", IndexOptions::DocsAndFreqs);
    state.set_length(4);
    state.set_unique_term_count(4);
    auto norm = similarity.compute_norm(state);
    EXPECT_TRUE(norm.has_value());

    auto raw_scorer = similarity.scorer(2.0F, collection("raw"), {});
    ASSERT_TRUE(raw_scorer.has_value());
    EXPECT_FLOAT_EQ(raw_scorer.value()->score(5.0F, 3), 10.0F);

    auto bm25_scorer = similarity.scorer(2.0F, collection("other"), single_term());
    ASSERT_TRUE(bm25_scorer.has_value());
    EXPECT_NE(dynamic_cast<const BM25Scorer *>(bm25_scorer.value().get()), nullptr);
}
