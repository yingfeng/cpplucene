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

export module lucene_core:multi_phrase_query;

import :types;
import std;

import :error;
import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :similarity;
import :term;
import :weight;

namespace lucene {

export class MultiPhraseQuery;

/// Scores a [`MultiPhraseQuery`].
///
/// Port of `PhraseQueryWeight` over `MultiPhraseQueryWeightBase`
/// (`core/search/multi_phrase_query.rs`), collapsed into one class the way the
/// port collapses the reference implementation's generic layers.
export class MultiPhraseWeight final : public Weight {
public:
    /// Builds the weight of `query`, reading the statistics of every term of
    /// every position.  A weight whose terms are all missing from the index
    /// builds no scorer.
    [[nodiscard]] static Result<std::unique_ptr<MultiPhraseWeight>> create(const MultiPhraseQuery &query, float boost,
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

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override;

private:
    MultiPhraseWeight(const MultiPhraseQuery &query, float boost, ScoreMode score_mode,
                      std::shared_ptr<const Similarity> similarity, std::shared_ptr<const SimScorer> sim_scorer);

    const MultiPhraseQuery &query_;
    float boost_;
    ScoreMode score_mode_;
    std::shared_ptr<const Similarity> similarity_;
    /// Null when none of the terms exists in the index.
    std::shared_ptr<const SimScorer> sim_scorer_;
};

/// A generalized [`PhraseQuery`](:phrase_query) where several terms may sit at the
/// same position and are treated as a disjunction.
///
/// Port of `MultiPhraseQuery` (`core/search/multi_phrase_query.rs`).
///
/// A document matches when, at every position of the query, one of the terms of
/// that position appears, and the positions of the chosen terms are at most
/// [`slop`] apart.
export class MultiPhraseQuery final : public Query {
public:
    /// Collects the positions and their terms, then builds the query.
    class Builder {
    public:
        Builder() = default;
        /// A builder that starts from the contents of `query`.
        explicit Builder(const MultiPhraseQuery &query);

        /// How many positions the terms may move apart; must not be negative.
        [[nodiscard]] VoidResult set_slop(int32_t slop);

        /// Adds `term` right after the last position added.
        [[nodiscard]] VoidResult add_term(Term term);

        /// Adds `terms` as a new position right after the last one added.
        [[nodiscard]] VoidResult add_terms(std::vector<Term> terms);

        /// Adds `terms` at `position`, which must not be before the previous one.
        [[nodiscard]] VoidResult add_terms_with_position(std::vector<Term> terms, int32_t position);

        [[nodiscard]] Result<MultiPhraseQuery> build();

    private:
        std::optional<std::string> field_;
        std::vector<std::vector<Term>> term_arrays_;
        std::vector<int32_t> positions_;
        int32_t slop_{0};
    };

    [[nodiscard]] static Builder builder();

    /// The number of positions the terms may move apart.
    [[nodiscard]] int32_t slop() const noexcept { return slop_; }
    /// The terms of each position.
    [[nodiscard]] const std::vector<std::vector<Term>> &term_arrays() const noexcept { return term_arrays_; }
    /// The position inside the phrase of each term array.
    [[nodiscard]] const std::vector<int32_t> &positions() const noexcept { return positions_; }
    /// The field of the query; `nullopt` for a query with no position.
    [[nodiscard]] const std::optional<std::string> &field() const noexcept { return field_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    /// A query with one position rewrites to the disjunction of its terms; an
    /// empty query matches nothing.
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `"a (b c) ? d"`, the form the reference implementation prints: `?` for a
    /// gap and `(b c)` for the terms of one position.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    MultiPhraseQuery(std::optional<std::string> field, std::vector<std::vector<Term>> term_arrays,
                     std::vector<int32_t> positions, int32_t slop);

    std::optional<std::string> field_;
    std::vector<std::vector<Term>> term_arrays_;
    std::vector<int32_t> positions_;
    int32_t slop_{0};
};

} // namespace lucene
