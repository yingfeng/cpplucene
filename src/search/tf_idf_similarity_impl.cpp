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

module lucene_core:tf_idf_similarity.impl;

import :tf_idf_similarity;

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

/// `byte4_to_int` applied to every byte, as the TF-IDF length table.
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

// ---- TFIDFSimilarityBase ----

Explanation TFIDFSimilarityBase::idf_explain(const CollectionStatistics &collection_stats,
                                             const TermStatistics &term_stats) const {
    const int64_t doc_freq = term_stats.doc_freq();
    const int64_t doc_count = collection_stats.doc_count();
    const float value = idf(doc_freq, doc_count);
    return Explanation::match_(value, "idf(doc_freq, doc_count)",
                               {Explanation::match_no_details(doc_freq,
                                                              "doc_freq, number of documents containing term"),
                                Explanation::match_no_details(
                                        doc_count, "doc_count, total number of documents with field")});
}

Explanation TFIDFSimilarityBase::idf_explain_phrase(const CollectionStatistics &collection_stats,
                                                    std::span<const TermStatistics> term_stats) const {
    double sum = 0.0;
    std::vector<Explanation> details;
    details.reserve(term_stats.size());
    for (const TermStatistics &stat : term_stats) {
        auto explained = idf_explain(collection_stats, stat);
        if (!explained.is_match()) {
            return explained;
        }
        sum += static_cast<double>(explanation_value(explained));
        details.push_back(std::move(explained));
    }
    return Explanation::match_(static_cast<float>(sum), "idf(), sum of:", std::move(details));
}

// ---- ClassicSimilarity ----

float ClassicSimilarity::tf(float freq) const {
    return std::sqrt(freq);
}

float ClassicSimilarity::idf(int64_t doc_freq, int64_t doc_count) const {
    // Add in int64_t precision before converting to double.
    const double num = static_cast<double>(doc_count + 1);
    const double den = static_cast<double>(doc_freq + 1);
    return static_cast<float>(std::log(num / den) + 1.0);
}

float ClassicSimilarity::length_norm(int32_t length) const {
    return static_cast<float>(1.0 / std::sqrt(static_cast<double>(length)));
}

Explanation ClassicSimilarity::idf_explain(const CollectionStatistics &collection_stats,
                                          const TermStatistics &term_stats) const {
    const int64_t doc_freq = term_stats.doc_freq();
    const int64_t doc_count = collection_stats.doc_count();
    const float value = idf(doc_freq, doc_count);
    return Explanation::match_(value, "idf, computed as log((docCount+1)/(docFreq+1)) + 1 from:",
                               {Explanation::match_no_details(doc_freq,
                                                              "docFreq, number of documents containing term"),
                                Explanation::match_no_details(
                                        doc_count, "docCount, total number of documents with field")});
}

std::string ClassicSimilarity::name() const {
    return "ClassicSimilarity";
}

// ---- TFIDFScorer ----

const std::array<float, 256> &TFIDFScorer::length_table() noexcept {
    return build_length_table();
}

TFIDFScorer::TFIDFScorer(float boost, Explanation idf, std::array<float, 256> norm_table,
                         std::shared_ptr<const TFIDFSimilarityBase> base)
        : boost_(boost),
          idf_(std::move(idf)),
          query_weight_(boost * explanation_value(idf_)),
          norm_table_(norm_table),
          base_(std::move(base)) {}

float TFIDFScorer::score(float freq, int64_t norm) const {
    const float raw = base_->tf(freq) * query_weight_;
    return raw * norm_table_[static_cast<uint8_t>(norm & 0xFF)];
}

Result<Explanation> TFIDFScorer::explain(const Explanation &freq, int64_t norm) const {
    std::vector<Explanation> details;
    if (boost_ != 1.0F) {
        details.push_back(Explanation::match_no_details(boost_, "boost"));
    }
    details.push_back(idf_);

    const float freq_value = explanation_value(freq);
    const float tf_value = base_->tf(freq_value);
    details.push_back(Explanation::match_(tf_value, std::format("tf(freq={}), with freq of:", freq_value),
                                          {freq}));

    const float norm_value = norm_table_[static_cast<uint8_t>(norm & 0xFF)];
    details.push_back(Explanation::match_no_details(norm_value, "fieldNorm"));

    return Explanation::match_(query_weight_ * tf_value * norm_value,
                               std::format("score(freq={}), product of:", freq_value), std::move(details));
}

// ---- TFIDFSimilarity ----

TFIDFSimilarity TFIDFSimilarity::create(std::shared_ptr<const TFIDFSimilarityBase> sub) {
    return TFIDFSimilarity(std::move(sub), true);
}

TFIDFSimilarity TFIDFSimilarity::with_discount_overlaps(std::shared_ptr<const TFIDFSimilarityBase> sub,
                                                        bool discount_overlaps) {
    return TFIDFSimilarity(std::move(sub), discount_overlaps);
}

TFIDFSimilarity TFIDFSimilarity::classic() {
    return create(std::make_shared<const ClassicSimilarity>());
}

TFIDFSimilarity TFIDFSimilarity::classic_with_discount_overlaps(bool discount_overlaps) {
    return with_discount_overlaps(std::make_shared<const ClassicSimilarity>(), discount_overlaps);
}

Result<std::unique_ptr<SimScorer>> TFIDFSimilarity::scorer(float boost,
                                                           const CollectionStatistics &collection_stats,
                                                           std::span<const TermStatistics> term_stats) const {
    Explanation idf = term_stats.size() == 1
                              ? sub_->idf_explain(collection_stats, term_stats[0])
                              : sub_->idf_explain_phrase(collection_stats, term_stats);
    if (!idf.is_match()) {
        return err(LuceneError::illegal_argument("cannot compute the idf of the query terms"));
    }

    const auto &lengths = build_length_table();
    std::array<float, 256> norm_table{};
    for (size_t i = 1; i < norm_table.size(); ++i) {
        norm_table[i] = sub_->length_norm(static_cast<int32_t>(lengths[i]));
    }
    // An encoded norm of zero is a field without terms; invert the last entry so
    // an empty field scores highest, like Lucene.
    norm_table[0] = 1.0F / norm_table[255];

    return std::unique_ptr<SimScorer>(new TFIDFScorer(boost, std::move(idf), norm_table, sub_));
}

std::string TFIDFSimilarity::name() const {
    return sub_->name();
}

// ---- factories ----

TFIDFSimilarity classic_similarity() {
    return TFIDFSimilarity::classic();
}

TFIDFSimilarity classic_similarity_with_discount_overlaps(bool discount_overlaps) {
    return TFIDFSimilarity::classic_with_discount_overlaps(discount_overlaps);
}

} // namespace lucene
