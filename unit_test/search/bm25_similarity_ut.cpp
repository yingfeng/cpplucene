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
#include <limits>
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.bm25_similarity;

import lucene_core;

using namespace lucene;

namespace {

/// 100 documents, field length 10 on average.
CollectionStatistics collection() {
    auto stats = CollectionStatistics::create("f", /*max_doc=*/100, /*doc_count=*/100,
                                              /*sum_total_term_freq=*/1000, /*sum_doc_freq=*/500);
    EXPECT_TRUE(stats.has_value());
    return stats.value();
}

TermStatistics term_stats(int64_t doc_freq = 10, int64_t total_term_freq = 20) {
    auto stats = TermStatistics::create(Term::from_text("f", "apple"), doc_freq, total_term_freq);
    EXPECT_TRUE(stats.has_value());
    return stats.value();
}

/// The score BM25 should produce, computed straight from the formula.
float expected_bm25(float boost, float k1, float b, float freq, float dl, float avgdl, float idf) {
    const double denominator = k1 * (1.0 - b + b * dl / avgdl);
    return static_cast<float>(static_cast<double>(boost) * idf * (1.0 - 1.0 / (1.0 + freq / denominator)));
}

} // namespace

TEST(SmallFloatTest, Byte4KeepsSmallValuesAndStaysMonotonic) {
    // Values below NUM_FREE_VALUES are stored verbatim.
    for (int32_t value = 0; value < SmallFloat::NUM_FREE_VALUES; ++value) {
        auto encoded = SmallFloat::int_to_byte4(value);
        ASSERT_TRUE(encoded.has_value());
        EXPECT_EQ(encoded.value(), value);
        EXPECT_EQ(SmallFloat::byte4_to_int(encoded.value()).value(), value);
    }
    EXPECT_EQ(SmallFloat::NUM_FREE_VALUES, 24);

    // Larger values round down and never reorder.
    int32_t previous_decoded = SmallFloat::NUM_FREE_VALUES;
    uint8_t previous_encoded = 0;
    for (int32_t value = SmallFloat::NUM_FREE_VALUES; value < 2000000; value += 7) {
        auto encoded = SmallFloat::int_to_byte4(value);
        ASSERT_TRUE(encoded.has_value()) << "value " << value;
        auto decoded = SmallFloat::byte4_to_int(encoded.value());
        ASSERT_TRUE(decoded.has_value());
        EXPECT_GE(decoded.value(), previous_decoded) << "value " << value;
        EXPECT_LE(decoded.value(), value) << "value " << value;
        EXPECT_GE(encoded.value(), previous_encoded);
        // At most 1/8 of the value is lost.
        if (value > 1000) {
            EXPECT_GE(decoded.value(), value - value / 8) << "value " << value;
        }
        previous_decoded = decoded.value();
        previous_encoded = encoded.value();
    }

    // The whole range round-trips through the encoding.
    for (int32_t byte = 0; byte < 256; ++byte) {
        auto decoded = SmallFloat::byte4_to_int(static_cast<uint8_t>(byte));
        ASSERT_TRUE(decoded.has_value());
        auto encoded = SmallFloat::int_to_byte4(decoded.value());
        ASSERT_TRUE(encoded.has_value());
        EXPECT_EQ(encoded.value(), byte) << "byte " << byte;
    }

    EXPECT_FALSE(SmallFloat::int_to_byte4(-1).has_value());
    EXPECT_FALSE(SmallFloat::long_to_int4(-1).has_value());
}

TEST(SmallFloatTest, Int4RoundTrips) {
    // 4 significant bits: only the multiples of the power of two survive.
    for (int64_t value = 0; value < 100000; value += 13) {
        auto encoded = SmallFloat::long_to_int4(value);
        ASSERT_TRUE(encoded.has_value());
        const int64_t decoded = SmallFloat::int4_to_long(encoded.value());
        EXPECT_LE(decoded, value);
        EXPECT_GE(decoded, value - value / 8) << "value " << value;
        auto re_encoded = SmallFloat::long_to_int4(decoded);
        ASSERT_TRUE(re_encoded.has_value());
        EXPECT_EQ(re_encoded.value(), encoded.value());
    }
}

TEST(SmallFloatTest, EightBitFloats) {
    EXPECT_FLOAT_EQ(SmallFloat::byte_3_15_to_float(0), 0.0F);
    EXPECT_FLOAT_EQ(SmallFloat::byte_to_float(0, 3, 15), 0.0F);

    // Negative values and zero both map to 0.
    EXPECT_EQ(SmallFloat::float_to_byte_3_15(-1.0F), 0);
    EXPECT_EQ(SmallFloat::float_to_byte_3_15(0.0F), 0);
    // The largest finite float saturates the encoding.
    EXPECT_EQ(SmallFloat::float_to_byte_3_15(std::numeric_limits<float>::max()), 255);

    for (int32_t byte = 1; byte < 256; ++byte) {
        const float decoded = SmallFloat::byte_3_15_to_float(static_cast<uint8_t>(byte));
        EXPECT_GT(decoded, 0.0F) << "byte " << byte;
        EXPECT_EQ(SmallFloat::float_to_byte_3_15(decoded), byte) << "byte " << byte;
        // The same byte through the generic pair.
        EXPECT_FLOAT_EQ(SmallFloat::byte_to_float(static_cast<uint8_t>(byte), 3, 15), decoded);
        EXPECT_EQ(SmallFloat::float_to_byte(decoded, 3, 15), byte);
    }

    // Values truncate downwards; the worst case is the bottom of a binade,
    // where roughly a fifth of the value can be lost.
    for (float value = 0.5F; value < 1000.0F; value *= 1.7F) {
        const uint8_t encoded = SmallFloat::float_to_byte_3_15(value);
        const float decoded = SmallFloat::byte_3_15_to_float(encoded);
        EXPECT_LE(decoded, value);
        EXPECT_GE(decoded, value * 0.8F) << "value " << value;
        // Decoding is monotonic in the byte.
        if (encoded < 255) {
            EXPECT_LE(decoded, SmallFloat::byte_3_15_to_float(static_cast<uint8_t>(encoded + 1)));
        }
    }
}

TEST(SimilarityTest, ComputesTheNorm) {
    auto similarity = BM25Similarity::default_similarity();
    ASSERT_TRUE(similarity.has_value());

    // Length 10 with 2 overlaps: the norm counts 8 terms, a free value.
    auto state = FieldInvertState::with_states(6, "f", IndexOptions::DocsAndFreqsAndPositions, /*position=*/1,
                                              /*length=*/10, /*num_overlap=*/2, /*offset=*/0,
                                              /*max_term_frequency=*/2, /*unique_term_count=*/7);
    auto norm = similarity.value().compute_norm(state);
    ASSERT_TRUE(norm.has_value());
    EXPECT_EQ(norm.value(), 8);

    // `Docs` uses the unique term count instead of the length.
    auto docs_state = FieldInvertState::with_states(6, "f", IndexOptions::Docs, 1, 10, 2, 0, 2, 7);
    auto docs_norm = similarity.value().compute_norm(docs_state);
    ASSERT_TRUE(docs_norm.has_value());
    EXPECT_EQ(docs_norm.value(), 7);

    // Lengths above the free range are encoded lossily but stay positive.
    auto long_state = FieldInvertState::with_states(6, "f", IndexOptions::DocsAndFreqs, 1, 300, 0, 0, 1, 300);
    auto long_norm = similarity.value().compute_norm(long_state);
    ASSERT_TRUE(long_norm.has_value());
    auto encoded = SmallFloat::int_to_byte4(300);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(long_norm.value(), static_cast<int64_t>(static_cast<int8_t>(encoded.value())));
    EXPECT_GT(long_norm.value(), 0);

    // Very long fields push the encoded byte past 127, and the norm is then
    // negative: the byte is sign extended, exactly as in Java.
    auto huge_state =
            FieldInvertState::with_states(6, "f", IndexOptions::DocsAndFreqs, 1, 1000000000, 0, 0, 1, 1000000000);
    auto huge_norm = similarity.value().compute_norm(huge_state);
    ASSERT_TRUE(huge_norm.has_value());
    auto huge_encoded = SmallFloat::int_to_byte4(1000000000);
    ASSERT_TRUE(huge_encoded.has_value());
    EXPECT_GE(huge_encoded.value(), 128);
    EXPECT_EQ(huge_norm.value(), static_cast<int64_t>(static_cast<int8_t>(huge_encoded.value())));
    EXPECT_LT(huge_norm.value(), 0);

    // A similarity that keeps overlap tokens counts the full length.
    auto keep_overlaps = BM25Similarity::with_discount(false);
    ASSERT_TRUE(keep_overlaps.has_value());
    auto full_norm = keep_overlaps.value().compute_norm(state);
    ASSERT_TRUE(full_norm.has_value());
    EXPECT_EQ(full_norm.value(), 10);
    EXPECT_FALSE(keep_overlaps.value().get_discount_overlaps());
}

TEST(SimilarityTest, ValidatesItsParameters) {
    EXPECT_FALSE(BM25Similarity::create(-1.0F, 0.75F, true).has_value());
    EXPECT_FALSE(BM25Similarity::create(std::numeric_limits<float>::infinity(), 0.75F, true).has_value());
    EXPECT_FALSE(BM25Similarity::create(1.2F, -0.5F, true).has_value());
    EXPECT_FALSE(BM25Similarity::create(1.2F, 1.5F, true).has_value());
    EXPECT_FALSE(BM25Similarity::create(1.2F, std::numeric_limits<float>::quiet_NaN(), true).has_value());
    EXPECT_TRUE(BM25Similarity::create(0.0F, 1.0F, false).has_value());

    auto defaults = BM25Similarity::default_similarity();
    ASSERT_TRUE(defaults.has_value());
    EXPECT_FLOAT_EQ(defaults.value().k1(), 1.2F);
    EXPECT_FLOAT_EQ(defaults.value().b(), 0.75F);
    EXPECT_TRUE(defaults.value().get_discount_overlaps());
    EXPECT_EQ(defaults.value().name(), "BM25(k1=1.2,b=0.75)");
}

TEST(SimilarityTest, ComputesTheInverseDocumentFrequency) {
    auto similarity = BM25Similarity::default_similarity();
    ASSERT_TRUE(similarity.has_value());

    // idf(1, 10) = log(1 + (10 - 1 + 0.5) / (1 + 0.5))
    const float expected = static_cast<float>(std::log(1.0 + 9.5 / 1.5));
    EXPECT_NEAR(similarity.value().idf(1, 10), expected, 1e-6);

    // Rare terms score higher than common ones.
    EXPECT_GT(similarity.value().idf(1, 100), similarity.value().idf(50, 100));
    EXPECT_GT(similarity.value().idf(50, 100), 0.0F);

    // A phrase sums the per-term factors.
    const auto stats = term_stats();
    auto phrase = similarity.value().idf_explain_phrase(collection(), std::span<const TermStatistics>(&stats, 1));
    EXPECT_TRUE(phrase.is_match());
    EXPECT_EQ(phrase.description(), "idf, sum of:");
    ASSERT_EQ(phrase.details().size(), 1);
    EXPECT_NEAR(static_cast<float>(number_as_f64(phrase.value())), similarity.value().idf(10, 100), 1e-6);

    auto single = similarity.value().idf_explain(collection(), stats);
    ASSERT_EQ(single.details().size(), 2);
    EXPECT_EQ(single.details()[0].description(), "n, number of documents containing term");
    EXPECT_EQ(number_as_i64(single.details()[0].value()), 10);
    EXPECT_EQ(single.details()[1].description(), "N, total number of documents with field");
    EXPECT_EQ(number_as_i64(single.details()[1].value()), 100);
}

TEST(SimilarityTest, ScoresLikeTheFormula) {
    auto similarity = BM25Similarity::default_similarity();
    ASSERT_TRUE(similarity.has_value());
    const auto stats = term_stats();

    auto scorer = similarity.value().scorer(/*boost=*/1.0F, collection(), std::span<const TermStatistics>(&stats, 1));
    ASSERT_TRUE(scorer.has_value()) << (scorer ? "" : scorer.error().to_string());

    const float avgdl = 1000.0F / 100.0F;  // 10
    const float idf = similarity.value().idf(10, 100);
    EXPECT_FLOAT_EQ(static_cast<BM25Scorer *>(scorer.value().get())->avgdl(), avgdl);

    // Norm byte for a document of length 10 decodes back to 10.
    auto norm = SmallFloat::int_to_byte4(10);
    ASSERT_TRUE(norm.has_value());
    const int64_t encoded_norm = norm.value();
    EXPECT_FLOAT_EQ(BM25Scorer::length_table()[norm.value()], 10.0F);

    for (const float freq : {1.0F, 2.0F, 7.5F, 100.0F}) {
        const float actual = scorer.value()->score(freq, encoded_norm);
        const float expected = expected_bm25(1.0F, 1.2F, 0.75F, freq, 10.0F, avgdl, idf);
        EXPECT_NEAR(actual, expected, 1e-5) << "freq " << freq;
    }

    // The boost multiplies the score linearly.
    auto boosted = similarity.value().scorer(2.0F, collection(), std::span<const TermStatistics>(&stats, 1));
    ASSERT_TRUE(boosted.has_value());
    EXPECT_NEAR(boosted.value()->score(3.0F, encoded_norm), 2.0F * scorer.value()->score(3.0F, encoded_norm), 1e-5);
}

TEST(SimilarityTest, ScoresAreMonotonic) {
    auto similarity = BM25Similarity::default_similarity();
    ASSERT_TRUE(similarity.has_value());
    const auto stats = term_stats();
    auto scorer = similarity.value().scorer(1.0F, collection(), std::span<const TermStatistics>(&stats, 1));
    ASSERT_TRUE(scorer.has_value());

    // Increasing frequency never lowers the score ...
    std::vector<int64_t> norms;
    for (const int32_t length : {1, 5, 10, 100, 1000}) {
        auto norm = SmallFloat::int_to_byte4(length);
        ASSERT_TRUE(norm.has_value());
        norms.push_back(norm.value());
    }
    for (const int64_t norm : norms) {
        float previous = -1.0F;
        for (int32_t freq = 1; freq <= 50; ++freq) {
            const float score = scorer.value()->score(static_cast<float>(freq), norm);
            EXPECT_GE(score, previous) << "freq " << freq << " norm " << norm;
            previous = score;
        }
    }

    // ... and a longer document never scores higher.
    for (int32_t freq = 1; freq <= 20; freq += 3) {
        float previous = std::numeric_limits<float>::max();
        for (const int64_t norm : norms) {
            const float score = scorer.value()->score(static_cast<float>(freq), norm);
            EXPECT_LE(score, previous) << "freq " << freq << " norm " << norm;
            previous = score;
        }
    }
}

TEST(SimilarityTest, ExplainsAScore) {
    auto similarity = BM25Similarity::default_similarity();
    ASSERT_TRUE(similarity.has_value());
    const auto stats = term_stats();
    auto scorer = similarity.value().scorer(1.0F, collection(), std::span<const TermStatistics>(&stats, 1));
    ASSERT_TRUE(scorer.has_value());

    auto norm = SmallFloat::int_to_byte4(10);
    ASSERT_TRUE(norm.has_value());
    const Explanation freq = Explanation::match_no_details(int32_t{3}, "freq, occurrences of term within document");
    auto explanation = scorer.value()->explain(freq, norm.value());
    ASSERT_TRUE(explanation.has_value()) << (explanation ? "" : explanation.error().to_string());

    EXPECT_TRUE(explanation.value().is_match());
    EXPECT_EQ(explanation.value().description(), "score(freq=3), computed as boost * idf * tf from:");
    // boost is 1, so the constant factors only hold the idf.
    ASSERT_EQ(explanation.value().details().size(), 2);
    EXPECT_EQ(explanation.value().details()[0].description(),
              "idf, computed as log(1 + (N - n + 0.5) / (n + 0.5)) from:");
    const Explanation &tf = explanation.value().details()[1];
    EXPECT_EQ(tf.description(), "tf, computed as freq / (freq + k1 * (1 - b + b * dl / avgdl)) from:");
    ASSERT_EQ(tf.details().size(), 5);
    EXPECT_EQ(tf.details()[1].description(), "k1, term saturation parameter");
    EXPECT_EQ(tf.details()[3].description(), "dl, length of field");

    // The explained value is the score itself.
    EXPECT_NEAR(static_cast<float>(number_as_f64(explanation.value().value())),
                scorer.value()->score(3.0F, norm.value()), 1e-6);

    // With a boost, a third factor appears.
    auto boosted = similarity.value().scorer(1.5F, collection(), std::span<const TermStatistics>(&stats, 1));
    ASSERT_TRUE(boosted.has_value());
    auto boosted_explanation = boosted.value()->explain(freq, norm.value());
    ASSERT_TRUE(boosted_explanation.has_value());
    ASSERT_EQ(boosted_explanation.value().details().size(), 3);
    EXPECT_EQ(boosted_explanation.value().details()[0].description(), "boost");
    EXPECT_FLOAT_EQ(static_cast<float>(number_as_f64(boosted_explanation.value().details()[0].value())), 1.5F);
}

TEST(ExplanationTest, RendersAsATree) {
    const Explanation root =
            Explanation::match_(2.5F, "sum of:",
                                {Explanation::match_no_details(1.0F, "first"),
                                 Explanation::match_(1.5F, "second", {Explanation::match_no_details(1.5F, "inner")})});
    EXPECT_TRUE(root.is_match());
    EXPECT_EQ(root.details().size(), 2);
    EXPECT_EQ(root.to_string(), "2.5 = sum of:\n  1 = first\n  1.5 = second\n    1.5 = inner\n");

    const Explanation no_match = Explanation::no_match_no_details("no match on required clause");
    EXPECT_FALSE(no_match.is_match());
    EXPECT_FLOAT_EQ(static_cast<float>(number_as_f64(no_match.value())), 0.0F);
    EXPECT_TRUE(Explanation::error_explanation("boom").is_match());
    EXPECT_EQ(Explanation::error_explanation("boom").to_string(), "0 = boom\n");

    EXPECT_EQ(root, root);
    EXPECT_NE(root, no_match);
}

TEST(StatisticsTest, ValidatesItsValues) {
    EXPECT_FALSE(CollectionStatistics::create("f", 0, 1, 1, 1).has_value());
    EXPECT_FALSE(CollectionStatistics::create("f", 10, 0, 1, 1).has_value());
    EXPECT_FALSE(CollectionStatistics::create("f", 10, 11, 20, 15).has_value());
    EXPECT_FALSE(CollectionStatistics::create("f", 10, 5, 20, 4).has_value());
    EXPECT_FALSE(CollectionStatistics::create("f", 10, 5, 0, 4).has_value());
    EXPECT_FALSE(CollectionStatistics::create("f", 10, 5, 4, 5).has_value());
    auto stats = CollectionStatistics::create("f", 10, 5, 20, 15);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats.value().field(), "f");
    EXPECT_EQ(stats.value().max_doc(), 10);
    EXPECT_EQ(stats.value().doc_count(), 5);
    EXPECT_EQ(stats.value().sum_total_term_freq(), 20);
    EXPECT_EQ(stats.value().sum_doc_freq(), 15);
    EXPECT_EQ(stats.value().to_string(),
              "CollectionStatistics[field=f, maxDoc=10, docCount=5, sumTotalTermFreq=20, sumDocFreq=15]");

    auto term = Term::from_text("f", "apple");
    EXPECT_FALSE(TermStatistics::create(term, 0, 1).has_value());
    EXPECT_FALSE(TermStatistics::create(term, 5, 0).has_value());
    EXPECT_FALSE(TermStatistics::create(term, 5, 4).has_value());
    auto term_stats_value = TermStatistics::create(term, 5, 7);
    ASSERT_TRUE(term_stats_value.has_value());
    EXPECT_EQ(term_stats_value.value().doc_freq(), 5);
    EXPECT_EQ(term_stats_value.value().total_term_freq(), 7);
    EXPECT_EQ(term_stats_value.value().term().text(), "apple");
}

TEST(TermTest, ComparesByFieldThenBytes) {
    const Term a = Term::from_text("a", "zzz");
    const Term b = Term::from_text("b", "aaa");
    const Term a1 = Term::from_text("a", "aaa");
    EXPECT_LT(a.compare_to(b), 0);
    EXPECT_GT(b.compare_to(a), 0);
    EXPECT_LT(a1.compare_to(a), 0);
    EXPECT_EQ(a.compare_to(Term::from_text("a", "zzz")), 0);
    EXPECT_EQ(a, Term::from_text("a", "zzz"));
    EXPECT_FALSE(a == Term::from_text("b", "zzz"));

    EXPECT_EQ(a.text(), "zzz");
    EXPECT_EQ(a.field(), "a");
    EXPECT_EQ(a.to_string(), "a:zzz");
    EXPECT_EQ(Term::from_empty("f").text(), "");

    // Bytes that are not UTF-8 fall back to the byte rendering.
    const Term invalid("f", BytesRef(std::vector<uint8_t>{0xFF, 0xFE}));
    EXPECT_EQ(invalid.text(), "[ff fe]");
}

TEST(FieldInvertStateTest, TracksTheCounters) {
    auto state = FieldInvertState::with_states(7, "body", IndexOptions::DocsAndFreqsAndPositions, 3, 12, 1, 40, 2,
                                               9);
    EXPECT_EQ(state.name(), "body");
    EXPECT_EQ(state.index_created_version_major(), 7);
    EXPECT_EQ(state.index_options(), IndexOptions::DocsAndFreqsAndPositions);
    EXPECT_EQ(state.position(), 3);
    EXPECT_EQ(state.length(), 12);
    EXPECT_EQ(state.num_overlap(), 1);
    EXPECT_EQ(state.offset(), 40);
    EXPECT_EQ(state.max_term_frequency(), 2);
    EXPECT_EQ(state.unique_term_count(), 9);

    state.reset();
    EXPECT_EQ(state.position(), -1);
    EXPECT_EQ(state.length(), 0);
    EXPECT_EQ(state.num_overlap(), 0);
    EXPECT_EQ(state.offset(), 0);
    EXPECT_EQ(state.max_term_frequency(), 0);
    EXPECT_EQ(state.unique_term_count(), 0);
    EXPECT_EQ(state.last_position(), 0);

    // The default constructor keeps the name/index options it was built with.
    FieldInvertState fresh(6, "title", IndexOptions::Docs);
    EXPECT_EQ(fresh.position(), 0);
    EXPECT_EQ(fresh.name(), "title");
}
