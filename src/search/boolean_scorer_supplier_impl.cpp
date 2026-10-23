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

module lucene_core:boolean_scorer_supplier.impl;

import :boolean_scorer_supplier;

import :block_max_conjunction_bulk_scorer;
import :block_max_conjunction_scorer;
import :bulk_scorer;
import :conjunction_bulk_scorer;
import :conjunction_utils;
import :max_score_bulk_scorer;
import :boolean_clause;
import :conjunction_utils;
import :constant_score_scorer;
import :disjunction_scorer;
import :doc_id_set_iterator;
import :error;
import :leaf_reader_context;
import :minimum_should_match_scorer;
import :req_excl_scorer;
import :req_opt_sum_scorer;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :types;

import std;

namespace lucene {

namespace {

/// Slot indices of the four clause lists.
constexpr size_t kMust = static_cast<size_t>(Occur::Must);
constexpr size_t kFilter = static_cast<size_t>(Occur::Filter);
constexpr size_t kShould = static_cast<size_t>(Occur::Should);
constexpr size_t kMustNot = static_cast<size_t>(Occur::MustNot);

/// Wraps a scorer whose scores must be ignored but whose `score()` is still
/// callable (the reference implementation's `FilterScorerImpl`).
///
/// `BooleanWeight` promises that `score()` is safe to call even when the query
/// only has `FILTER` clauses but scores are needed, so the filter's own score
/// must not leak out.
class FilterScorer final : public Scorer {
public:
    explicit FilterScorer(std::unique_ptr<Scorer> inner) : inner_(std::move(inner)) {}

    [[nodiscard]] int32_t doc_id() const override { return inner_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return inner_->next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return inner_->advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return inner_->cost(); }

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return inner_->scoring_doc_id(); }
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override { return 0.0F; }
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return inner_->has_two_phase_iterator(); }
    [[nodiscard]] Result<bool> matches() override { return inner_->matches(); }
    [[nodiscard]] float match_cost() const override { return inner_->match_cost(); }
    [[nodiscard]] int32_t approx_doc_id() const override { return inner_->approx_doc_id(); }
    [[nodiscard]] Result<int32_t> approx_next_doc() override { return inner_->approx_next_doc(); }
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override { return inner_->approx_advance(target); }
    [[nodiscard]] Result<int64_t> approx_cost() const override { return inner_->approx_cost(); }
    [[nodiscard]] Result<float> score() override { return 0.0F; }
    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override {
        return inner_->set_min_competitive_score(min_score);
    }

private:
    std::unique_ptr<Scorer> inner_;
};

} // namespace

Result<std::unique_ptr<BooleanScorerSupplier>> BooleanScorerSupplier::create(
        std::array<std::vector<std::unique_ptr<ScorerSupplier>>, 4> subs, ScoreMode score_mode, int32_t min_should_match,
        int32_t max_doc) {
    if (min_should_match < 0) {
        return err(LuceneError::illegal_argument("min_should_match must be >= 0, got " +
                                                 std::to_string(min_should_match)));
    }
    return std::unique_ptr<BooleanScorerSupplier>(
            new BooleanScorerSupplier(std::move(subs), score_mode, min_should_match, max_doc));
}

Result<std::unique_ptr<Scorer>> BooleanScorerSupplier::required(int64_t lead_cost, const LeafReaderContext &context,
                                                                const IndexSearcher &searcher) {
    std::vector<std::unique_ptr<Scorer>> filters;
    std::vector<std::unique_ptr<Scorer>> musts;
    for (auto &supplier : subs_[kFilter]) {
        if (supplier == nullptr) {
            continue;
        }
        auto scorer = supplier->get(lead_cost, context, searcher);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        if (scorer.value() != nullptr) {
            filters.push_back(std::move(scorer.value()));
        }
    }
    for (auto &supplier : subs_[kMust]) {
        if (supplier == nullptr) {
            continue;
        }
        auto scorer = supplier->get(lead_cost, context, searcher);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        if (scorer.value() != nullptr) {
            musts.push_back(std::move(scorer.value()));
        }
    }

    const size_t total = filters.size() + musts.size();
    if (total == 0) {
        return std::unique_ptr<Scorer>{};
    }
    if (total == 1) {
        if (!musts.empty()) {
            return std::move(musts.front());
        }
        // Only a filter clause, but scores may still be requested: BooleanWeight
        // promises `score()` is callable, so keep it from leaking the filter's
        // own score.
        if (!score_mode_needs_scores(score_mode_)) {
            return std::move(filters.front());
        }
        return std::unique_ptr<Scorer>(new FilterScorer(std::move(filters.front())));
    }

    // With several scoring clauses, a top-level conjunction that is allowed to
    // score can prune whole blocks that cannot hold a competitive document.
    if (score_mode_ == ScoreMode::TopScores && musts.size() > 1 && top_level_scoring_clause_) {
        auto block_max = BlockMaxConjunctionScorer::prepare(std::move(musts));
        if (!block_max.has_value()) {
            return std::unexpected(std::move(block_max.error()));
        }
        if (filters.empty()) {
            return std::unique_ptr<Scorer>(std::move(block_max.value()));
        }
        std::vector<std::unique_ptr<Scorer>> all;
        all.reserve(filters.size() + 1);
        for (auto &scorer : filters) {
            all.push_back(std::move(scorer));
        }
        all.push_back(std::move(block_max.value()));
        std::vector<size_t> scoring{all.size() - 1};
        return ConjunctionUtils::intersect_scorers(std::move(all), std::move(scoring));
    }

    std::vector<std::unique_ptr<Scorer>> all;
    all.reserve(total);
    for (auto &scorer : filters) {
        all.push_back(std::move(scorer));
    }
    std::vector<size_t> scoring;
    scoring.reserve(musts.size());
    for (auto &scorer : musts) {
        all.push_back(std::move(scorer));
        scoring.push_back(all.size() - 1);
    }
    return ConjunctionUtils::intersect_scorers(std::move(all), std::move(scoring));
}

Result<std::unique_ptr<Scorer>> BooleanScorerSupplier::optional(Occur occur, int32_t min_should_match,
                                                                int64_t lead_cost, const LeafReaderContext &context,
                                                                const IndexSearcher &searcher) {
    std::vector<std::unique_ptr<Scorer>> scorers;
    for (auto &supplier : subs_[static_cast<size_t>(occur)]) {
        if (supplier == nullptr) {
            continue;
        }
        auto scorer = supplier->get(lead_cost, context, searcher);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        if (scorer.value() != nullptr) {
            scorers.push_back(std::move(scorer.value()));
        }
    }
    if (scorers.empty()) {
        return std::unique_ptr<Scorer>{};
    }
    if (scorers.size() == 1) {
        return std::move(scorers.front());
    }
    if (min_should_match <= 1) {
        return DisjunctionSumScorer::create(std::move(scorers));
    }
    return MinimumShouldMatchScorer::create(std::move(scorers), min_should_match);
}

Result<std::unique_ptr<Scorer>> BooleanScorerSupplier::exclude(std::unique_ptr<Scorer> scorer, int64_t lead_cost,
                                                               const LeafReaderContext &context,
                                                               const IndexSearcher &searcher) {
    if (subs_[kMustNot].empty()) {
        return scorer;
    }
    std::vector<std::unique_ptr<Scorer>> prohibited;
    for (auto &supplier : subs_[kMustNot]) {
        if (supplier == nullptr) {
            continue;
        }
        auto excluded_scorer = supplier->get(lead_cost, context, searcher);
        if (!excluded_scorer.has_value()) {
            return std::unexpected(std::move(excluded_scorer.error()));
        }
        if (excluded_scorer.value() != nullptr) {
            prohibited.push_back(std::move(excluded_scorer.value()));
        }
    }
    if (prohibited.empty()) {
        return scorer;
    }

    std::unique_ptr<Scorer> excluded;
    if (prohibited.size() == 1) {
        excluded = std::move(prohibited.front());
    } else {
        auto disjunction = DisjunctionSumScorer::create(std::move(prohibited));
        if (!disjunction.has_value()) {
            return std::unexpected(std::move(disjunction.error()));
        }
        excluded = std::move(disjunction.value());
    }

    if (scorer == nullptr) {
        // Nothing positive to filter; the caller decides what that means.
        return std::unique_ptr<Scorer>{};
    }
    return ReqExclScorer::create(std::move(scorer), std::move(excluded));
}

Result<std::unique_ptr<Scorer>> BooleanScorerSupplier::get_internal(int64_t lead_cost,
                                                                    const LeafReaderContext &context,
                                                                    const IndexSearcher &searcher) {
    const bool filter_empty = subs_[kFilter].empty();
    const bool must_empty = subs_[kMust].empty();

    // Pure disjunction (only SHOULD / MUST_NOT clauses): one optional part, the
    // prohibited part wraps it.
    if (filter_empty && must_empty) {
        auto optional_scorer = optional(Occur::Should, min_should_match_, lead_cost, context, searcher);
        if (!optional_scorer.has_value()) {
            return std::unexpected(std::move(optional_scorer.error()));
        }
        if (optional_scorer.value() == nullptr) {
            return std::unique_ptr<Scorer>{};
        }
        return exclude(std::move(optional_scorer.value()), lead_cost, context, searcher);
    }

    auto required_scorer = required(lead_cost, context, searcher);
    if (!required_scorer.has_value()) {
        return std::unexpected(std::move(required_scorer.error()));
    }
    auto with_exclusions = exclude(std::move(required_scorer.value()), lead_cost, context, searcher);
    if (!with_exclusions.has_value()) {
        return std::unexpected(std::move(with_exclusions.error()));
    }
    if (with_exclusions.value() == nullptr) {
        return std::unique_ptr<Scorer>{};
    }

    auto optional_scorer = optional(Occur::Should, min_should_match_, lead_cost, context, searcher);
    if (!optional_scorer.has_value()) {
        return std::unexpected(std::move(optional_scorer.error()));
    }

    if (min_should_match_ > 0) {
        if (optional_scorer.value() == nullptr) {
            // Fewer optional clauses than required: nothing can match.
            return std::unique_ptr<Scorer>{};
        }
        // The optional side must match too, so both parts are required and both
        // score.
        std::vector<std::unique_ptr<Scorer>> parts;
        parts.push_back(std::move(with_exclusions.value()));
        parts.push_back(std::move(optional_scorer.value()));
        return ConjunctionUtils::intersect_scorers(std::move(parts), {0, 1});
    }

    if (optional_scorer.value() == nullptr) {
        return std::move(with_exclusions.value());
    }
    return ReqOptSumScorer::create(std::move(with_exclusions.value()), std::move(optional_scorer.value()));
}

Result<std::unique_ptr<Scorer>> BooleanScorerSupplier::get(int64_t lead_cost, const LeafReaderContext &context,
                                                           const IndexSearcher &searcher) {
    auto scorer = get_internal(lead_cost, context, searcher);
    if (!scorer.has_value()) {
        return std::unexpected(std::move(scorer.error()));
    }
    if (scorer.value() == nullptr) {
        return scorer;
    }

    // No scoring clause but scores are wanted: a constant score lets the
    // collector skip work, and keeps `score()` meaningful.
    if (score_mode_ == ScoreMode::TopScores && subs_[kMust].empty() && subs_[kShould].empty() &&
        scorer.value()->has_two_phase_iterator() == TwoPhaseState::No) {
        return std::unique_ptr<Scorer>(ConstantScoreScorer::from_disi(0.0F, score_mode_, std::move(scorer.value())));
    }
    return scorer;
}

Result<int64_t> BooleanScorerSupplier::cost(const LeafReaderContext &context, const IndexSearcher &searcher) {
    int64_t total = 0;
    for (auto &subs : subs_) {
        for (auto &supplier : subs) {
            if (supplier == nullptr) {
                continue;
            }
            auto sub_cost = supplier->cost(context, searcher);
            if (!sub_cost.has_value()) {
                return std::unexpected(std::move(sub_cost.error()));
            }
            total += sub_cost.value();
        }
    }
    return total;
}

VoidResult BooleanScorerSupplier::set_top_level_scoring_clause() {
    top_level_scoring_clause_ = true;
    // Propagate only when a single clause scores: that clause's scores go
    // straight to the collector, which is what enables its early termination.
    const size_t scoring_clauses = subs_[kShould].size() + subs_[kMust].size();
    if (scoring_clauses != 1) {
        return {};
    }
    for (auto &supplier : subs_[kShould]) {
        if (auto set = supplier->set_top_level_scoring_clause(); !set.has_value()) {
            return set;
        }
    }
    for (auto &supplier : subs_[kMust]) {
        if (auto set = supplier->set_top_level_scoring_clause(); !set.has_value()) {
            return set;
        }
    }
    return {};
}


Result<std::unique_ptr<BulkScorer>> BooleanScorerSupplier::bulk_scorer(const LeafReaderContext &context,
                                                                     const IndexSearcher &searcher) {
    // A pure disjunction that is allowed to score can use MAXSCORE, so that low
    // scoring clauses stop driving the iteration.
    if (subs_[kMust].empty() && subs_[kMustNot].empty() && subs_[kShould].size() >= 2 &&
        min_should_match_ <= 1 && score_mode_ == ScoreMode::TopScores) {
        auto disjunction = disjunction_bulk_scorer(context, searcher);
        if (disjunction.has_value() && disjunction.value() != nullptr) {
            return disjunction;
        }
        if (!disjunction.has_value()) {
            return std::unexpected(std::move(disjunction.error()));
        }
        // Otherwise fall through to the generic path.
    }

    // The specialised bulk scorers only cover a plain conjunction: optional
    // clauses and exclusions change the shape of the iterator too much.
    if (!subs_[kShould].empty() || !subs_[kMustNot].empty() || subs_[kMust].empty()) {
        return ScorerSupplier::bulk_scorer(context, searcher);
    }
    const size_t clause_count = subs_[kMust].size() + subs_[kFilter].size();
    if (clause_count < 2) {
        return ScorerSupplier::bulk_scorer(context, searcher);
    }

    std::vector<std::unique_ptr<Scorer>> filters;
    std::vector<std::unique_ptr<Scorer>> musts;
    std::vector<Scorer *> all;
    for (auto &supplier : subs_[kFilter]) {
        if (supplier == nullptr) {
            continue;
        }
        auto scorer = supplier->get(std::numeric_limits<int64_t>::max(), context, searcher);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        if (scorer.value() != nullptr) {
            all.push_back(scorer.value().get());
            filters.push_back(std::move(scorer.value()));
        }
    }
    for (auto &supplier : subs_[kMust]) {
        if (supplier == nullptr) {
            continue;
        }
        auto scorer = supplier->get(std::numeric_limits<int64_t>::max(), context, searcher);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        if (scorer.value() != nullptr) {
            all.push_back(scorer.value().get());
            musts.push_back(std::move(scorer.value()));
        }
    }
    if (musts.empty() || musts.size() + filters.size() < 2) {
        return ScorerSupplier::bulk_scorer(context, searcher);
    }
    // Two-phase clauses need confirmation by a collector, which the conjunction
    // bulk scorers do not do.
    for (const Scorer *scorer : all) {
        if (scorer->has_two_phase_iterator() == TwoPhaseState::Yes) {
            return ScorerSupplier::bulk_scorer(context, searcher);
        }
    }

    if (score_mode_ == ScoreMode::TopScores && musts.size() > 1) {
        // Every clause has to score for the block-max window to be an upper
        // bound, so the filters join as constant-score clauses.
        std::vector<std::unique_ptr<Scorer>> scorable;
        scorable.reserve(musts.size() + filters.size());
        for (auto &scorer : musts) {
            scorable.push_back(std::move(scorer));
        }
        for (auto &filter : filters) {
            std::unique_ptr<DocIdSetIterator> iterator(filter.release());
            scorable.push_back(std::unique_ptr<Scorer>(
                    ConstantScoreScorer::from_disi(0.0F, ScoreMode::Complete, std::move(iterator))));
        }
        auto bulk = BlockMaxConjunctionBulkScorer::create(max_doc_, std::move(scorable));
        if (!bulk.has_value()) {
            return std::unexpected(std::move(bulk.error()));
        }
        return std::unique_ptr<BulkScorer>(std::move(bulk.value()));
    }

    auto bulk = ConjunctionBulkScorer::create(std::move(musts), std::move(filters));
    if (!bulk.has_value()) {
        return std::unexpected(std::move(bulk.error()));
    }
    return std::unique_ptr<BulkScorer>(std::move(bulk.value()));
}


Result<std::unique_ptr<BulkScorer>> BooleanScorerSupplier::disjunction_bulk_scorer(const LeafReaderContext &context,
                                                                                 const IndexSearcher &searcher) {
    std::vector<std::unique_ptr<Scorer>> optional;
    for (auto &supplier : subs_[kShould]) {
        if (supplier == nullptr) {
            continue;
        }
        auto scorer = supplier->get(std::numeric_limits<int64_t>::max(), context, searcher);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        if (scorer.value() != nullptr) {
            optional.push_back(std::move(scorer.value()));
        }
    }
    if (optional.size() < 2) {
        // A single matching clause (or none) has nothing to prune.
        return std::unique_ptr<BulkScorer>{};
    }

    // The filters only decide which documents may match; they never score.
    std::vector<std::unique_ptr<Scorer>> filters;
    for (auto &supplier : subs_[kFilter]) {
        if (supplier == nullptr) {
            continue;
        }
        auto scorer = supplier->get(std::numeric_limits<int64_t>::max(), context, searcher);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        if (scorer.value() != nullptr) {
            filters.push_back(std::move(scorer.value()));
        }
    }

    std::unique_ptr<Scorer> filter;
    if (filters.size() == 1) {
        filter = std::move(filters.front());
    } else if (!filters.empty()) {
        std::vector<size_t> no_scoring{};
        auto conjunction = ConjunctionUtils::intersect_scorers(std::move(filters), no_scoring);
        if (!conjunction.has_value()) {
            return std::unexpected(std::move(conjunction.error()));
        }
        filter = std::move(conjunction.value());
    }

    auto bulk = MaxScoreBulkScorer::create(max_doc_, std::move(optional), std::move(filter));
    if (!bulk.has_value()) {
        return std::unexpected(std::move(bulk.error()));
    }
    return std::unique_ptr<BulkScorer>(std::move(bulk.value()));
}

} // namespace lucene
