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

export module lucene_core:tf_idf_similarity;

import :types;
import std;

import :collection_statistics;
import :error;
import :explanation;
import :similarity;
import :term_statistics;

namespace lucene {

/// The three pluggable parts of the TF-IDF scoring model.
///
/// Port of the `TFIDFSimilarityBase` trait
/// (`core/search/similarities_impl/tf_idf_similarity.rs`).  A
/// [`TFIDFSimilarity`] is a [`Similarity`] that delegates `tf`, `idf` and
/// `length_norm` to one of these, exactly like Lucene's `TFIDFSimilarity` does
/// for subclasses that only override those three methods.
export class TFIDFSimilarityBase {
public:
    virtual ~TFIDFSimilarityBase() = default;

    /// The score factor of a term with `freq` occurrences in a document.
    [[nodiscard]] virtual float tf(float freq) const = 0;

    /// The score factor of a term appearing in `doc_freq` of `doc_count`
    /// documents.
    [[nodiscard]] virtual float idf(int64_t doc_freq, int64_t doc_count) const = 0;

    /// The length normalization factor of a field with `length` terms.
    [[nodiscard]] virtual float length_norm(int32_t length) const = 0;

    /// The idf of one term, with its sub-explanations.
    [[nodiscard]] virtual Explanation idf_explain(const CollectionStatistics &collection_stats,
                                                 const TermStatistics &term_stats) const;

    /// The idf of a phrase: the sum of the per-term factors.
    [[nodiscard]] virtual Explanation idf_explain_phrase(const CollectionStatistics &collection_stats,
                                                         std::span<const TermStatistics> term_stats) const;

    /// A human readable name, reported by [`TFIDFSimilarity::name`].
    [[nodiscard]] virtual std::string name() const = 0;
};

/// The historical TF-IDF factors: `tf = sqrt(freq)`,
/// `idf = log((doc_count + 1) / (doc_freq + 1)) + 1` and
/// `length_norm = 1 / sqrt(length)`.
///
/// Port of `ClassicSimilarity`
/// (`core/search/similarities_impl/classic_similarity.rs`).  In the reference
/// implementation this type is only the *factor set*; the [`Similarity`] is
/// built by [`classic_similarity`].
export class ClassicSimilarity final : public TFIDFSimilarityBase {
public:
    [[nodiscard]] float tf(float freq) const override;

    [[nodiscard]] float idf(int64_t doc_freq, int64_t doc_count) const override;

    [[nodiscard]] float length_norm(int32_t length) const override;

    [[nodiscard]] Explanation idf_explain(const CollectionStatistics &collection_stats,
                                          const TermStatistics &term_stats) const override;

    [[nodiscard]] std::string name() const override;
};

/// The collection-level weight of TF-IDF, with the norm table precomputed.
///
/// Port of `TFIDFScorer`.  `norm_table[0]` is `1 / norm_table[255]`, so an empty
/// field gets the largest normalization factor, exactly like Lucene.
export class TFIDFScorer final : public SimScorer {
public:
    TFIDFScorer(float boost, Explanation idf, std::array<float, 256> norm_table,
                std::shared_ptr<const TFIDFSimilarityBase> base);

    [[nodiscard]] float score(float freq, int64_t norm) const override;

    [[nodiscard]] Result<Explanation> explain(const Explanation &freq, int64_t norm) const override;

    /// `boost * idf`.
    [[nodiscard]] float query_weight() const noexcept { return query_weight_; }
    [[nodiscard]] float boost() const noexcept { return boost_; }
    [[nodiscard]] const Explanation &idf() const noexcept { return idf_; }
    /// The decoded norms, indexed by the low byte of the encoded norm.
    [[nodiscard]] const std::array<float, 256> &norm_table() const noexcept { return norm_table_; }

    /// `byte4_to_int` applied to every byte.
    [[nodiscard]] static const std::array<float, 256> &length_table() noexcept;

private:
    float boost_{1.0F};
    Explanation idf_;
    float query_weight_{1.0F};
    std::array<float, 256> norm_table_{};
    std::shared_ptr<const TFIDFSimilarityBase> base_;
};

/// The vector space model, parameterized by a [`TFIDFSimilarityBase`].
///
/// Port of `TFIDFSimilarity`
/// (`core/search/similarities_impl/tf_idf_similarity.rs`): the practical scoring
/// function is `sum over terms of tf(freq) * idf^2 * boost * length_norm`.
export class TFIDFSimilarity final : public Similarity {
public:
    /// A similarity that delegates to `sub`, discounting overlaps.
    [[nodiscard]] static TFIDFSimilarity create(std::shared_ptr<const TFIDFSimilarityBase> sub);
    /// A similarity that delegates to `sub`.
    [[nodiscard]] static TFIDFSimilarity with_discount_overlaps(
            std::shared_ptr<const TFIDFSimilarityBase> sub, bool discount_overlaps);

    /// [`ClassicSimilarity`] factors, discounting overlaps.
    [[nodiscard]] static TFIDFSimilarity classic();
    /// [`ClassicSimilarity`] factors.
    [[nodiscard]] static TFIDFSimilarity classic_with_discount_overlaps(bool discount_overlaps);

    [[nodiscard]] bool get_discount_overlaps() const override { return discount_overlaps_; }

    [[nodiscard]] Result<std::unique_ptr<SimScorer>> scorer(
            float boost, const CollectionStatistics &collection_stats,
            std::span<const TermStatistics> term_stats) const override;

    /// The name of the factor set, e.g. `ClassicSimilarity`.
    [[nodiscard]] std::string name() const override;

    /// The factors this similarity scores with.
    [[nodiscard]] const TFIDFSimilarityBase &base() const noexcept { return *sub_; }

private:
    TFIDFSimilarity(std::shared_ptr<const TFIDFSimilarityBase> sub, bool discount_overlaps) noexcept
        : sub_(std::move(sub)), discount_overlaps_(discount_overlaps) {}

    std::shared_ptr<const TFIDFSimilarityBase> sub_;
    bool discount_overlaps_{true};
};

/// [`ClassicSimilarity`] factors, discounting overlaps.
///
/// Port of the `new` factory of `classic_similarity`.
[[nodiscard]] TFIDFSimilarity classic_similarity();

/// [`ClassicSimilarity`] factors.
///
/// Port of the `with_discount_overlaps` factory of `classic_similarity`.
[[nodiscard]] TFIDFSimilarity classic_similarity_with_discount_overlaps(bool discount_overlaps);

} // namespace lucene
