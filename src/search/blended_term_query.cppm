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

export module lucene_core:blended_term_query;

import :types;
import std;

import :error;
import :query;
import :query_visitor;
import :score_mode;
import :term;
import :term_states;
import :weight;

namespace lucene {

export class IndexSearcher;

/// A query that blends index statistics across several terms.
///
/// Port of `BlendedTermQuery` (`core/search/blended_term_query.rs`).  Every term
/// scores with the same document frequency and total term frequency (the maximum
/// of the document frequencies and the sum of the total term frequencies), so
/// that a rare term does not dominate: the use case is search-time synonym
/// expansion and cross-field search.
///
/// **Deviation:** the reference implementation expresses the two ways of
/// combining the terms as `RewriteMethod` trait objects; the port uses
/// [`BlendedRewriteMethod`], a small value type.
export class BlendedTermQuery final : public Query {
public:
    /// How the blended terms are combined into one query.
    enum class RewriteMethod : uint8_t {
        /// A boolean query of the term queries: matching several fields is
        /// considered better than a good match in one field.
        kBoolean = 0,
        /// A [`DisjunctionMaxQuery`](lucene_core:disjunction_max_query) with the
        /// given tie breaker: the best field dominates.
        kDisjunctionMax = 1,
    };

    /// The tie breaker [`RewriteMethod::kDisjunctionMax`] uses by default.
    static constexpr float kDefaultTieBreakerMultiplier = 0.01F;

    /// Collects the terms to blend.
    class Builder {
    public:
        Builder() = default;

        /// Selects how the terms are combined; the default is
        /// [`RewriteMethod::kBoolean`].
        void set_rewrite_method(RewriteMethod rewrite_method) noexcept { rewrite_method_ = rewrite_method; }

        /// Selects [`RewriteMethod::kDisjunctionMax`] with `tie_breaker_multiplier`.
        void set_disjunction_max_rewrite(float tie_breaker_multiplier) noexcept {
            rewrite_method_ = RewriteMethod::kDisjunctionMax;
            tie_breaker_multiplier_ = tie_breaker_multiplier;
        }

        /// Adds `term` with `boost` and no pre-computed term states.
        [[nodiscard]] VoidResult add(Term term, float boost = 1.0F);

        /// Adds `term` with `boost`; `states` may be null, in which case they are
        /// built during [`BlendedTermQuery::rewrite`].
        [[nodiscard]] VoidResult add_with_term_states(Term term, float boost, std::shared_ptr<TermStates> states);

        /// Builds the query; it must have at least one term.
        [[nodiscard]] Result<BlendedTermQuery> build();

    private:
        std::vector<Term> terms_{};
        std::vector<float> boosts_{};
        std::vector<std::shared_ptr<TermStates>> states_{};
        RewriteMethod rewrite_method_{RewriteMethod::kBoolean};
        float tie_breaker_multiplier_{kDefaultTieBreakerMultiplier};
    };

    /// The terms being blended, in term order.
    [[nodiscard]] const std::vector<Term> &terms() const noexcept { return terms_; }
    /// The boost of each term.
    [[nodiscard]] const std::vector<float> &boosts() const noexcept { return boosts_; }
    /// How the terms are combined.
    [[nodiscard]] RewriteMethod rewrite_method() const noexcept { return rewrite_method_; }
    /// The tie breaker of a [`RewriteMethod::kDisjunctionMax`] query.
    [[nodiscard]] float tie_breaker_multiplier() const noexcept { return tie_breaker_multiplier_; }

    // ---- Query ----

    /// **Deviation:** the reference implementation fails here too (a blended query
    /// must be rewritten first); this port's `MultiTermQuery::create_weight` and
    /// `IndexSearcher::rewrite` both rewrite before asking for a weight, so the
    /// error only shows up when the query is used without rewriting.
    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    /// Blends the statistics of the terms and combines them into one query,
    /// following [`rewrite_method`].
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `Blended(term\ term\ ...)`, the form the reference implementation prints.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    BlendedTermQuery(std::vector<Term> terms, std::vector<float> boosts, std::vector<std::shared_ptr<TermStates>> states,
                     RewriteMethod rewrite_method, float tie_breaker_multiplier);

    std::vector<Term> terms_{};
    std::vector<float> boosts_{};
    std::vector<std::shared_ptr<TermStates>> states_{};
    RewriteMethod rewrite_method_{RewriteMethod::kBoolean};
    float tie_breaker_multiplier_{kDefaultTieBreakerMultiplier};
};

} // namespace lucene
