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

export module lucene_core:term_query;

import :types;
import std;

import :error;
import :explanation;
import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :similarity;
import :term;
import :term_states;
import :terms_enum;
import :weight;

namespace lucene {

export class Matches;
export class MatchesIterator;

/// A query that matches the documents containing a term.
///
/// Port of `TermQuery` (`core/search/term_query.rs`).  The term states may be
/// supplied up front (`with_term_state`), which lets a caller reuse the term
/// dictionary lookups across several queries; they are only reused when they
/// were built for the same top-level reader.
export class TermQuery final : public Query {
public:
    explicit TermQuery(Term term);
    TermQuery(Term term, std::shared_ptr<TermStates> per_reader_term_state);

    [[nodiscard]] const Term &term() const noexcept { return term_; }

    /// The term states this query was created with, or null.
    [[nodiscard]] const std::shared_ptr<TermStates> &term_states() const noexcept { return per_reader_term_state_; }

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    Term term_;
    std::shared_ptr<TermStates> per_reader_term_state_;
};

/// The [`Weight`] of a [`TermQuery`].
///
/// Port of `TermWeight` (`core/search/term_query.rs`).
///
/// **Deviation:** the reference implementation stores the parent query as an `Arc<Query>` it owns;
/// here the weight borrows it (see [`Weight`]).
export class TermWeight final : public Weight {
public:
    /// Builds the weight: collects the statistics the similarity needs and
    /// creates the collection-level scorer.
    [[nodiscard]] static Result<std::unique_ptr<TermWeight>> create(const Query &parent_query, const Term &term,
                                                                   const IndexSearcher &searcher, ScoreMode score_mode,
                                                                   float boost,
                                                                   std::shared_ptr<TermStates> term_states);

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override;

    [[nodiscard]] const Query &get_query() const override { return *parent_query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                          const IndexSearcher &searcher) override;

    /// Counts the matches of a leaf that has no deletions by its document
    /// frequency, without building a scorer.
    [[nodiscard]] Result<int32_t> count(const LeafReaderContext &context, const IndexSearcher &searcher) override;

    /// The matches of `doc` in the field of the term.
    [[nodiscard]] Result<std::unique_ptr<Matches>> matches(const LeafReaderContext &context, int32_t doc,
                                                           const IndexSearcher &searcher) override;

    /// The matches iterator of `doc`, or a null pointer when the term does not
    /// occur in that document.
    [[nodiscard]] Result<std::unique_ptr<MatchesIterator>> matches_iterator(const LeafReaderContext &context,
                                                                            int32_t doc) const;

    /// A term weight only depends on segment-immutable structures.
    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override {
        (void)context;
        return true;
    }

    [[nodiscard]] const Term &term() const noexcept { return term_; }
    [[nodiscard]] ScoreMode score_mode() const noexcept { return score_mode_; }
    [[nodiscard]] const std::shared_ptr<Similarity> &similarity() const noexcept { return similarity_; }
    [[nodiscard]] const std::shared_ptr<SimScorer> &sim_scorer() const noexcept { return sim_scorer_; }
    [[nodiscard]] const std::shared_ptr<TermStates> &term_states() const noexcept { return term_states_; }

private:
    TermWeight(const Query &parent_query, Term term, std::shared_ptr<Similarity> similarity,
               std::shared_ptr<SimScorer> sim_scorer, std::shared_ptr<TermStates> term_states, ScoreMode score_mode);

    /// A `TermsEnum` positioned on the weight's term in `context`, or null when
    /// the term does not occur in that leaf.
    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> get_terms_enum(const LeafReaderContext &context) const;

    /// Whether the term is absent from the leaf (used as an invariant check).
    [[nodiscard]] Result<bool> term_not_in_reader(const LeafReaderContext &context) const;

    const Query *parent_query_;
    Term term_;
    std::shared_ptr<Similarity> similarity_;
    std::shared_ptr<SimScorer> sim_scorer_;
    std::shared_ptr<TermStates> term_states_;
    ScoreMode score_mode_;
};

/// The [`ScorerSupplier`] of a [`TermWeight`].
///
/// Port of `TermScorerSupplier` (`core/search/term_query.rs`).  It resolves the
/// leaf's term state on first use, so the cost can be known before a scorer is
/// built.
export class TermScorerSupplier final : public ScorerSupplier {
public:
    TermScorerSupplier(bool top_level_scoring_clause, std::shared_ptr<TermStates> term_states,
                       PrepareState prepare_state, Term term, std::shared_ptr<SimScorer> sim_scorer,
                       ScoreMode score_mode);

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t lead_cost, const LeafReaderContext &context,
                                                      const IndexSearcher &searcher) override;

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext &context, const IndexSearcher &searcher) override;

    [[nodiscard]] VoidResult set_top_level_scoring_clause() override;

private:
    /// Resolves the leaf's term state once; `true` when the term occurs there.
    [[nodiscard]] Result<bool> prepare(const LeafReaderContext &context);

    bool top_level_scoring_clause_;
    std::shared_ptr<TermStates> term_states_;
    PrepareState prepare_state_;
    Term term_;
    std::shared_ptr<SimScorer> sim_scorer_;
    ScoreMode score_mode_;
    std::unique_ptr<TermsEnum> terms_enum_{};
    bool prepared_{false};
    bool present_{false};
};

/// Builds the scorer of the term in `context`.
///
/// The reference implementation's `TermWeight::build_term_scorer`: with a `terms_enum` it scores the
/// postings, without one the leaf cannot match and the result is an empty
/// constant-score scorer.
///
/// `ScoreMode::TopScores` goes through the indexed impacts, so the scorer can
/// prune non-competitive blocks; `top_level_scoring_clause` decides whether the
/// collector's minimum competitive score can drive that pruning.
///
/// **Deviation:** terms enums that cannot serve impacts (fake dictionaries,
/// term vectors) fall back to the postings path instead of failing.
[[nodiscard]] Result<std::unique_ptr<Scorer>> build_term_scorer(const LeafReaderContext &context,
                                                               TermsEnum *terms_enum, const Term &term,
                                                               std::shared_ptr<SimScorer> sim_scorer,
                                                               ScoreMode score_mode,
                                                               bool top_level_scoring_clause = false);

} // namespace lucene
