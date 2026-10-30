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

module lucene_core:constant_score_query.impl;

import :constant_score_query;

import std;

import :bulk_scorer;
import :constant_score_scorer;
import :error;
import :filter_leaf_collector;
import :search_filters;
import :leaf_collector;
import :explanation;
import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :scorable;
import :scorer;
import :two_phase_iterator;
import :scorer_supplier;
import :segment_cacheable;
import :types;
import :weight;

namespace lucene {

namespace {

/// Presents a scorer's confirmation step as a [`TwoPhaseIterator`], so a
/// constant-score wrapper keeps it.
///
/// The reference implementation gets this for free: its `ConstantScoreScorer`
/// constructor takes the scorer itself and reads `twoPhaseIterator()` from it.
class ScorerTwoPhase final : public TwoPhaseIterator {
public:
    explicit ScorerTwoPhase(std::unique_ptr<Scorer> scorer) : scorer_(std::move(scorer)) {}

    [[nodiscard]] int32_t approx_doc_id() const override { return scorer_->approx_doc_id(); }
    [[nodiscard]] Result<int32_t> approx_next_doc() override { return scorer_->approx_next_doc(); }
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override { return scorer_->approx_advance(target); }
    [[nodiscard]] Result<int64_t> approx_cost() const override { return scorer_->approx_cost(); }
    [[nodiscard]] Result<bool> matches() override { return scorer_->matches(); }
    [[nodiscard]] float match_cost() const override { return scorer_->match_cost(); }

private:
    std::unique_ptr<Scorer> scorer_;
};

/// A [`LeafCollector`] that hides the score of the wrapped query: whatever the
/// inner bulk scorer reports, the collector sees the constant score.
class ConstantScoreLeafCollector final : public FilterLeafCollector {
public:
    ConstantScoreLeafCollector(LeafCollector &inner, float score)
            : FilterLeafCollector(inner), score_(score) {}

    [[nodiscard]] VoidResult set_scorer(Scorable &scorer) override {
        ConstantScorable constant(scorer, score_);
        return inner().set_scorer(constant);
    }

private:
    /// Reports the constant score, and forwards everything else.
    class ConstantScorable final : public FilterScorable {
    public:
        ConstantScorable(Scorable &inner, float score) : FilterScorable(inner), score_(score) {}

        [[nodiscard]] Result<float> score() override { return score_; }

    private:
        float score_;
    };

    float score_;
};

/// Port of `ConstantBulkScorer`: the wrapped query keeps its own top-level
/// scorer (a conjunction bulk scorer, a boolean scorer, ...) and every hit is
/// reported with the constant score.
class ConstantBulkScorer final : public BulkScorer {
public:
    ConstantBulkScorer(std::unique_ptr<BulkScorer> inner, float score) : inner_(std::move(inner)), score_(score) {}

    [[nodiscard]] Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                        int32_t max) override {
        ConstantScoreLeafCollector wrapped(collector, score_);
        return inner_->score(wrapped, accept_docs, min, max);
    }

    [[nodiscard]] Result<int64_t> cost() override { return inner_->cost(); }

private:
    std::unique_ptr<BulkScorer> inner_;
    float score_;
};

/// The scorer supplier of [`ConstantScoreQuery`]: asks the wrapped query for a
/// scorer and gives every document the same score.
class ConstantScoreScorerSupplier final : public ScorerSupplier {
public:
    ConstantScoreScorerSupplier(std::unique_ptr<ScorerSupplier> inner, float score, ScoreMode score_mode)
            : inner_(std::move(inner)), score_(score), score_mode_(score_mode) {}

    /// Scores exhaustively through the wrapped query's own bulk scorer, so that
    /// wrapping a query in a constant score does not throw away its optimized
    /// top-level scorer.
    [[nodiscard]] Result<std::unique_ptr<BulkScorer>> bulk_scorer(const LeafReaderContext &context,
                                                                 const IndexSearcher &searcher) override {
        if (!score_mode_is_exhaustive(score_mode_)) {
            return ScorerSupplier::bulk_scorer(context, searcher);
        }
        auto inner = inner_->bulk_scorer(context, searcher);
        if (!inner.has_value()) {
            return std::unexpected(std::move(inner.error()));
        }
        if (inner.value() == nullptr) {
            return std::unique_ptr<BulkScorer>{};
        }
        return std::unique_ptr<BulkScorer>(new ConstantBulkScorer(std::move(inner.value()), score_));
    }

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t lead_cost, const LeafReaderContext &context,
                                                      const IndexSearcher &searcher) override {
        auto inner = inner_->get(lead_cost, context, searcher);
        if (!inner.has_value()) {
            return std::unexpected(std::move(inner.error()));
        }
        if (inner.value() == nullptr) {
            return std::unique_ptr<Scorer>{};
        }
        std::unique_ptr<Scorer> scorer = std::move(inner.value());
        if (scorer->has_two_phase_iterator() == TwoPhaseState::No) {
            return ConstantScoreScorer::from_disi(score_, score_mode_, std::move(scorer));
        }
        // The wrapped scorer confirms its candidates in `matches()`, and that
        // confirmation has to survive the constant scoring: dropping it would
        // collect every candidate of the approximation (e.g. a disjunction that
        // requires `minimum_should_match`, whose iterator yields the union).
        return ConstantScoreScorer::from_tpi(score_, score_mode_, std::make_unique<ScorerTwoPhase>(std::move(scorer)));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext &context, const IndexSearcher &searcher) override {
        return inner_->cost(context, searcher);
    }

    [[nodiscard]] VoidResult set_top_level_scoring_clause() override {
        return inner_->set_top_level_scoring_clause();
    }

private:
    std::unique_ptr<ScorerSupplier> inner_;
    float score_;
    ScoreMode score_mode_;
};

/// The weight of [`ConstantScoreQuery`].
class ConstantScoreWeight final : public Weight {
public:
    ConstantScoreWeight(const Query &parent_query, std::shared_ptr<Weight> inner, ScoreMode score_mode, float score)
            : parent_query_(&parent_query), inner_(std::move(inner)), score_mode_(score_mode), score_(score) {}

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override {
        return inner_->is_cacheable(context);
    }

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override {
        auto inner = inner_->explain(context, doc, searcher);
        if (!inner.has_value()) {
            return std::unexpected(std::move(inner.error()));
        }
        const std::string description =
                std::format("ConstantScore({})", inner_->get_query().to_string("").value_or(""));
        if (!inner.value().is_match()) {
            return Explanation::no_match(std::format("no matching docs in {}, doc={}", description, doc), {});
        }
        return Explanation::match_(score_, description, {inner.value()});
    }

    [[nodiscard]] const Query &get_query() const override { return *parent_query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                          const IndexSearcher &searcher) override {
        auto inner = inner_->scorer_supplier(context, searcher);
        if (!inner.has_value()) {
            return std::unexpected(std::move(inner.error()));
        }
        if (inner.value() == nullptr) {
            // The wrapped query has no scorer for this leaf, so neither has the
            // constant score query.
            return std::unique_ptr<ScorerSupplier>{};
        }
        return std::unique_ptr<ScorerSupplier>(new ConstantScoreScorerSupplier(std::move(inner.value()), score_, score_mode_));
    }

    [[nodiscard]] Result<int32_t> count(const LeafReaderContext &context, const IndexSearcher &searcher) override {
        return inner_->count(context, searcher);
    }

private:
    const Query *parent_query_;
    std::shared_ptr<Weight> inner_;
    ScoreMode score_mode_;
    float score_;
};

} // namespace

ConstantScoreQuery::ConstantScoreQuery(std::shared_ptr<Query> query) : query_(std::move(query)) {
    debug_check(query_ != nullptr, "a constant score query needs a wrapped query");
}

Result<std::unique_ptr<Weight>> ConstantScoreQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                  float boost) const {
    if (query_ == nullptr) {
        return err(LuceneError::illegal_state("ConstantScoreQuery has no wrapped query"));
    }
    // An exhaustive score mode means the wrapped query does not have to score:
    // telling it so keeps its dynamic pruning available.  Otherwise it is asked
    // for `TOP_DOCS`, since the constant score query itself does not rank.
    const ScoreMode inner_score_mode =
            score_mode_is_exhaustive(score_mode) ? ScoreMode::CompleteNoScores : ScoreMode::TopDocs;
    auto inner = query_->create_weight(searcher, inner_score_mode, 1.0F);
    if (!inner.has_value()) {
        return std::unexpected(std::move(inner.error()));
    }
    if (!score_mode_needs_scores(score_mode)) {
        // Nobody looks at the score, so the wrapper is pure overhead: hand back
        // the wrapped weight, which knows its own bulk scorers.
        return std::unique_ptr<Weight>(std::move(inner.value()));
    }
    std::unique_ptr<Weight> weight(new ConstantScoreWeight(*this, std::move(inner.value()), score_mode, boost));
    return weight;
}

Result<std::shared_ptr<Query>> ConstantScoreQuery::rewrite(const IndexSearcher &searcher) const {
    if (query_ == nullptr) {
        return std::shared_ptr<Query>{};
    }
    auto rewritten = query_->rewrite(searcher);
    if (!rewritten.has_value()) {
        return std::unexpected(std::move(rewritten.error()));
    }
    if (rewritten.value() == nullptr) {
        return std::shared_ptr<Query>{};
    }
    // ConstantScore(ConstantScore(q)) is ConstantScore(q).
    if (const auto *nested = dynamic_cast<const ConstantScoreQuery *>(rewritten.value().get()); nested != nullptr) {
        return rewritten.value();
    }
    return std::shared_ptr<Query>(new ConstantScoreQuery(rewritten.value()));
}

VoidResult ConstantScoreQuery::visit(QueryVisitor &visitor) const {
    if (query_ == nullptr) {
        return {};
    }
    return query_->visit(visitor);
}

Result<std::string> ConstantScoreQuery::to_string(std::string_view field) const {
    if (query_ == nullptr) {
        return std::string("ConstantScore()");
    }
    auto inner = query_->to_string(field);
    if (!inner.has_value()) {
        return std::unexpected(std::move(inner.error()));
    }
    return std::format("ConstantScore({})", inner.value());
}

bool ConstantScoreQuery::equals(const Query &other) const {
    const auto *constant_score = dynamic_cast<const ConstantScoreQuery *>(&other);
    if (constant_score == nullptr || query_ == nullptr || constant_score->query_ == nullptr) {
        return false;
    }
    return query_->equals(*constant_score->query_);
}

size_t ConstantScoreQuery::hash() const {
    if (query_ == nullptr) {
        return 0;
    }
    return query_->hash() * 31 + 0x9E3779B9;
}

} // namespace lucene
