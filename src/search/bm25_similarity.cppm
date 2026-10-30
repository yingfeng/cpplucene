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

export module lucene_core:bm25_similarity;

import :types;
import std;

import :collection_statistics;
import :error;
import :explanation;
import :similarity;
import :term_statistics;

namespace lucene {

/// The BM25 term-frequency factor, `weight - weight / (1 + freq * norm_inverse)`.
///
/// `freq / (freq + norm)` is rewritten as `1 - 1 / (1 + freq * 1/norm)` so the
/// computation stays monotonic without promoting to doubles, and
/// `weight * (...)` becomes `weight - weight / (...)`.  The scorers that inline
/// this call it here as well, so both produce the same bits.
[[nodiscard]] inline float bm25_tf_score(float weight, float norm_inverse, float freq) noexcept {
    return weight - weight / (1.0F + freq * norm_inverse);
}

/// Okapi BM25.
///
/// Port of `BM25Similarity`
/// (`core/search/similarities_impl/bm25_similarity.rs`), with `k1 = 1.2`,
/// `b = 0.75` and overlap discounting by default.
///
/// Score layout: `boost * idf * (1 - 1 / (1 + freq / (k1 * (1 - b + b * dl / avgdl))))`,
/// where `dl` is decoded from the norm byte through the BM25 length table.
export class BM25Similarity final : public Similarity {
public:
    /// BM25 with the given parameters.
    [[nodiscard]] static Result<BM25Similarity> create(float k1, float b, bool discount_overlaps);
    /// BM25 with the given parameters and `discount_overlaps = true`.
    [[nodiscard]] static Result<BM25Similarity> with_k1_b(float k1, float b);
    /// BM25 with the default parameters and the given `discount_overlaps`.
    [[nodiscard]] static Result<BM25Similarity> with_discount(bool discount_overlaps);
    /// BM25 with the default parameters.
    [[nodiscard]] static Result<BM25Similarity> default_similarity();

    /// `log(1 + (doc_count - doc_freq + 0.5) / (doc_freq + 0.5))`.
    [[nodiscard]] float idf(int64_t doc_freq, int64_t doc_count) const noexcept;

    /// `sum_total_term_freq / doc_count`.
    [[nodiscard]] float avg_field_length(const CollectionStatistics &collection_stats) const noexcept;

    /// The idf factor of one term, with its sub-explanations.
    [[nodiscard]] Explanation idf_explain(const CollectionStatistics &collection_stats,
                                          const TermStatistics &term_stats) const;

    /// The idf factor of a phrase: the sum of the per-term factors.
    [[nodiscard]] Explanation idf_explain_phrase(const CollectionStatistics &collection_stats,
                                                 std::span<const TermStatistics> term_stats) const;

    [[nodiscard]] float k1() const noexcept { return k1_; }
    [[nodiscard]] float b() const noexcept { return b_; }

    [[nodiscard]] bool get_discount_overlaps() const override { return discount_overlaps_; }

    [[nodiscard]] Result<std::unique_ptr<SimScorer>> scorer(
            float boost, const CollectionStatistics &collection_stats,
            std::span<const TermStatistics> term_stats) const override;

    /// `BM25(k1=1.2,b=0.75)`.
    [[nodiscard]] std::string name() const override;

private:
    BM25Similarity(float k1, float b, bool discount_overlaps) noexcept
        : k1_(k1), b_(b), discount_overlaps_(discount_overlaps) {}

    float k1_{1.2F};
    float b_{0.75F};
    bool discount_overlaps_{true};
};

/// The collection-level weight of BM25, with the norm table precomputed.
export class BM25Scorer final : public SimScorer {
public:
    /// `idf` is kept whole so [`explain`] can show it.
    BM25Scorer(float boost, float k1, float b, Explanation idf, float avgdl, std::array<float, 256> cache);

    [[nodiscard]] float score(float freq, int64_t norm) const override;

    [[nodiscard]] Result<Explanation> explain(const Explanation &freq, int64_t norm) const override;

    /// `boost * idf`.
    [[nodiscard]] float weight() const noexcept { return weight_; }
    [[nodiscard]] float avgdl() const noexcept { return avgdl_; }
    [[nodiscard]] const Explanation &idf() const noexcept { return idf_; }

    /// `1 / (k1 * (1 - b + b * dl / avgdl))`, indexed by the norm byte.
    [[nodiscard]] const float *norm_inverse_cache() const noexcept { return cache_.data(); }

    /// The decoded `byte4` lengths, `LENGTH_TABLE[i] = byte4_to_int(i)`.
    [[nodiscard]] static const std::array<float, 256> &length_table() noexcept;

private:
    [[nodiscard]] Explanation explain_tf(const Explanation &freq, int64_t norm) const;
    [[nodiscard]] std::vector<Explanation> explain_constant_factors() const;

    float boost_{1.0F};
    float k1_{1.2F};
    float b_{0.75F};
    Explanation idf_;
    float avgdl_{0.0F};
    std::array<float, 256> cache_{};
    float weight_{0.0F};
};

} // namespace lucene
