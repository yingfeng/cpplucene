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

module lucene_core:bm25_similarity.impl;

import :bm25_similarity;

import :collection_statistics;
import :error;
import :explanation;
import :number;
import :similarity;
import :small_float;
import :term_statistics;
import :types;

import std;

namespace lucene {

namespace {

/// `byte4_to_int` applied to every byte, as the BM25 length table.
const std::array<float, 256> &build_length_table() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            // Every byte is a valid byte4 value.
            values[i] = static_cast<float>(SmallFloat::byte4_to_int(static_cast<uint8_t>(i)).value());
        }
        return values;
    }();
    return table;
}

/// The value of an explanation as a float.
float explanation_value(const Explanation &explanation) {
    return static_cast<float>(number_as_f64(explanation.value()));
}

} // namespace

// ---- BM25Similarity ----

Result<BM25Similarity> BM25Similarity::create(float k1, float b, bool discount_overlaps) {
    if (!std::isfinite(k1) || k1 < 0.0F) {
        return err(LuceneError::illegal_argument(
                "illegal k1 value: " + number_to_string(k1) + ", must be a non-negative finite value"));
    }
    if (std::isnan(b) || b < 0.0F || b > 1.0F) {
        return err(LuceneError::illegal_argument("illegal b value: " + number_to_string(b) +
                                                 ", must be between 0 and 1"));
    }
    return BM25Similarity(k1, b, discount_overlaps);
}

Result<BM25Similarity> BM25Similarity::with_k1_b(float k1, float b) { return create(k1, b, true); }

Result<BM25Similarity> BM25Similarity::with_discount(bool discount_overlaps) {
    return create(1.2F, 0.75F, discount_overlaps);
}

Result<BM25Similarity> BM25Similarity::default_similarity() { return create(1.2F, 0.75F, true); }

float BM25Similarity::idf(int64_t doc_freq, int64_t doc_count) const noexcept {
    const double numerator = static_cast<double>(doc_count - doc_freq) + 0.5;
    const double denominator = static_cast<double>(doc_freq) + 0.5;
    return static_cast<float>(std::log(1.0 + numerator / denominator));
}

float BM25Similarity::avg_field_length(const CollectionStatistics &collection_stats) const noexcept {
    return static_cast<float>(static_cast<double>(collection_stats.sum_total_term_freq()) /
                              static_cast<double>(collection_stats.doc_count()));
}

Explanation BM25Similarity::idf_explain(const CollectionStatistics &collection_stats,
                                       const TermStatistics &term_stats) const {
    const int64_t df = term_stats.doc_freq();
    const int64_t doc_count = collection_stats.doc_count();
    const float idf_value = idf(df, doc_count);

    std::vector<Explanation> details;
    details.push_back(Explanation::match_no_details(Number(df), "n, number of documents containing term"));
    details.push_back(
            Explanation::match_no_details(Number(doc_count), "N, total number of documents with field"));
    return Explanation::match_(Number(idf_value), "idf, computed as log(1 + (N - n + 0.5) / (n + 0.5)) from:",
                               std::move(details));
}

Explanation BM25Similarity::idf_explain_phrase(const CollectionStatistics &collection_stats,
                                              std::span<const TermStatistics> term_stats) const {
    double idf_sum = 0.0;  // sum into f64 before casting to f32
    std::vector<Explanation> details;
    details.reserve(term_stats.size());
    for (const TermStatistics &stat : term_stats) {
        details.push_back(idf_explain(collection_stats, stat));
        idf_sum += static_cast<double>(idf(stat.doc_freq(), collection_stats.doc_count()));
    }
    return Explanation::match_(Number(static_cast<float>(idf_sum)), "idf, sum of:", std::move(details));
}

Result<std::unique_ptr<SimScorer>> BM25Similarity::scorer(
        float boost, const CollectionStatistics &collection_stats,
        std::span<const TermStatistics> term_stats) const {
    // We need to know the idf value and average document length of the field.
    Explanation idf = term_stats.size() == 1 ? idf_explain(collection_stats, term_stats.front())
                                             : idf_explain_phrase(collection_stats, term_stats);

    const float avgdl = avg_field_length(collection_stats);

    const auto &length_table = BM25Scorer::length_table();
    std::array<float, 256> cache{};
    for (size_t i = 0; i < cache.size(); ++i) {
        cache[i] = 1.0F / (k1_ * ((1.0F - b_) + b_ * length_table[i] / avgdl));
    }

    return std::unique_ptr<SimScorer>(new BM25Scorer(boost, k1_, b_, std::move(idf), avgdl, cache));
}

std::string BM25Similarity::name() const {
    return "BM25(k1=" + number_to_string(k1_) + ",b=" + number_to_string(b_) + ")";
}

// ---- BM25Scorer ----

BM25Scorer::BM25Scorer(float boost, float k1, float b, Explanation idf, float avgdl,
                       std::array<float, 256> cache)
    : boost_(boost), k1_(k1), b_(b), idf_(std::move(idf)), avgdl_(avgdl), cache_(cache) {
    weight_ = boost * explanation_value(idf_);
}

const std::array<float, 256> &BM25Scorer::length_table() noexcept { return build_length_table(); }

float BM25Scorer::score(float freq, int64_t norm) const {
    return bm25_tf_score(weight_, cache_[static_cast<uint8_t>(norm)], freq);
}

std::vector<Explanation> BM25Scorer::explain_constant_factors() const {
    std::vector<Explanation> subs;
    if (boost_ != 1.0F) {
        subs.push_back(Explanation::match_no_details(boost_, "boost"));
    }
    subs.push_back(idf_);
    return subs;
}

Explanation BM25Scorer::explain_tf(const Explanation &freq, int64_t norm) const {
    std::vector<Explanation> subs;
    subs.reserve(5);
    const float freq_value = explanation_value(freq);
    subs.push_back(freq);
    subs.push_back(Explanation::match_no_details(k1_, "k1, term saturation parameter"));

    const float doclen = length_table()[static_cast<uint8_t>(norm)];
    subs.push_back(Explanation::match_no_details(b_, "b, length normalization parameter"));

    if ((norm & 0xFF) > 39) {
        subs.push_back(Explanation::match_no_details(doclen, "dl, length of field (approximate)"));
    } else {
        subs.push_back(Explanation::match_no_details(doclen, "dl, length of field"));
    }

    subs.push_back(Explanation::match_no_details(avgdl_, "avgdl, average length of field"));

    const float norm_inverse = 1.0F / (k1_ * ((1.0F - b_) + b_ * doclen / avgdl_));
    const float tf_value = 1.0F - 1.0F / (1.0F + freq_value * norm_inverse);

    return Explanation::match_(tf_value, "tf, computed as freq / (freq + k1 * (1 - b + b * dl / avgdl)) from:",
                               std::move(subs));
}

Result<Explanation> BM25Scorer::explain(const Explanation &freq, int64_t norm) const {
    std::vector<Explanation> subs = explain_constant_factors();
    const float freq_value = explanation_value(freq);
    const std::string description = "score(freq=" + number_to_string(freq.value()) +
                                    "), computed as boost * idf * tf from:";
    subs.push_back(explain_tf(freq, norm));

    const float norm_inverse = cache_[static_cast<uint8_t>(norm)];
    const float score_value = weight_ - weight_ / (1.0F + freq_value * norm_inverse);
    // not using "product of" since the rewrite that we do in score()
    // introduces a small rounding error that CheckHits complains about
    return Explanation::match_(score_value, description, std::move(subs));
}

} // namespace lucene
