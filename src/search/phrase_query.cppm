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

export module lucene_core:phrase_query;

import :types;
import std;

import :collection_statistics;
import :error;
import :index_searcher;
import :leaf_reader_context;
import :phrase_scorer;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :similarity;
import :term;
import :weight;

namespace lucene {

export class PhraseQuery;

/// Scores a [`PhraseQuery`]: one [`PhraseScorer`] per leaf, built from the
/// postings of every term of the phrase.
///
/// Port of `PhraseQueryWeight` + `PhraseWeight` (`core/search/phrase_query.rs`,
/// `core/search/phrase_weight.rs`), collapsed into one class the way the port
/// collapses the reference implementation's generic layers.
export class PhraseWeight final : public Weight {
public:
    /// Builds the weight of `query`, reading the collection and term statistics
    /// the similarity needs.  A weight that found a term missing in the whole
    /// index builds no scorer, i.e. it matches nothing.
    [[nodiscard]] static Result<std::unique_ptr<PhraseWeight>> create(const PhraseQuery &query, float boost,
                                                                     ScoreMode score_mode,
                                                                     const IndexSearcher &searcher);

    // ---- Weight ----

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override;

    [[nodiscard]] const Query &get_query() const override;

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher &searcher) override;

    /// **Deviation:** sub-linear counting would need the block-max machinery the
    /// port does not have, so the count is unknown.
    [[nodiscard]] Result<int32_t> count(const LeafReaderContext &context, const IndexSearcher &searcher) override;

    /// A phrase weight depends on the reader statistics, so it is not cached
    /// across readers.
    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override;

private:
    PhraseWeight(const PhraseQuery &query, float boost, ScoreMode score_mode,
                 std::shared_ptr<const Similarity> similarity, std::shared_ptr<const SimScorer> sim_scorer);

    /// Builds the scorer of one leaf, or a null pointer when the leaf cannot
    /// match.
    [[nodiscard]] Result<std::unique_ptr<PhraseScorer>> scorer_for(const LeafReaderContext &context) const;

    const PhraseQuery &query_;
    float boost_;
    ScoreMode score_mode_;
    std::shared_ptr<const Similarity> similarity_;
    /// Null when a term of the phrase is missing from the whole index.
    std::shared_ptr<const SimScorer> sim_scorer_;
};

/// A query for the documents where a sequence of terms appears at consecutive
/// positions, allowing up to [`slop`] positions of movement.
///
/// Port of `PhraseQuery` (`core/search/phrase_query.rs`).
///
/// Terms are added with a position inside the phrase, which is how stop words can
/// leave a gap (`"a ? c"`), and several terms may share a position, in which case
/// every one of them has to be present (use `MultiPhraseQuery` when only one of
/// them has to be present).
export class PhraseQuery final : public Query {
public:
    /// The default slop, i.e. an exact phrase.
    static constexpr int32_t DEFAULT_SLOP = 0;

    /// A phrase of `terms` on `field`, consecutive from position `0`.
    [[nodiscard]] static Result<PhraseQuery> create(std::string field, std::vector<Term> terms);

    /// [`create`], with `slop` positions of movement allowed.
    [[nodiscard]] static Result<PhraseQuery> with_slop(std::string field, std::vector<Term> terms, int32_t slop);

    /// A phrase with the positions of the terms given explicitly.
    [[nodiscard]] static Result<PhraseQuery> with_positions(std::string field, std::vector<Term> terms,
                                                            std::vector<int32_t> positions, int32_t slop);

    /// A phrase whose field is the field of the terms, consecutive from `0`.
    [[nodiscard]] static Result<PhraseQuery> from_terms(std::vector<Term> terms);

    /// [`from_terms`], with `slop` positions of movement allowed.
    [[nodiscard]] static Result<PhraseQuery> from_terms_with_slop(std::vector<Term> terms, int32_t slop);

    /// A phrase whose field is the field of the terms, with the positions given
    /// explicitly.
    [[nodiscard]] static Result<PhraseQuery> from_terms_and_positions(std::vector<Term> terms,
                                                                     std::vector<int32_t> positions, int32_t slop);

    /// A phrase of `texts` on `field`, consecutive from position `0`.
    [[nodiscard]] static Result<PhraseQuery> from_term_texts(std::string field, std::vector<std::string> texts);

    /// The field of the phrase.
    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    /// The terms of the phrase, in insertion order.
    [[nodiscard]] const std::vector<Term> &terms() const noexcept { return terms_; }
    /// The position inside the phrase of each term.
    [[nodiscard]] const std::vector<int32_t> &positions() const noexcept { return positions_; }
    /// How many positions the terms may move apart.
    [[nodiscard]] int32_t slop() const noexcept { return slop_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    /// A phrase of one term is that term; a phrase of no term matches nothing.
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `"a b"~2`, with `?` for a gap and `|` between terms at the same
    /// position, the form the reference implementation prints.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    PhraseQuery(std::string field, std::vector<Term> terms, std::vector<int32_t> positions, int32_t slop);

    std::string field_;
    std::vector<Term> terms_;
    std::vector<int32_t> positions_;
    int32_t slop_{0};
};

} // namespace lucene
