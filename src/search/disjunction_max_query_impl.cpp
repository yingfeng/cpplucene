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

module lucene_core:disjunction_max_query.impl;

import :disjunction_max_query;

import std;

import :boolean_clause;
import :boolean_query;
import :disjunction_max_scorer;
import :error;
import :explanation;
import :index_searcher;
import :leaf_reader_context;
import :match_no_docs_query;
import :number;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :types;
import :weight;

namespace lucene {

namespace {

/// How many disjuncts a query may have before it stops being cacheable: large
/// disjunctions must not be used as a way around the clause limit.
constexpr size_t kCacheableDisjunctLimit = 16;

/// Suppliers of the disjuncts, wrapped into one scorer.
///
/// Port of `ScorerSupplierImpl` (`core/search/disjunction_max_query.rs`).
class DisjunctionMaxScorerSupplier final : public ScorerSupplier {
public:
    DisjunctionMaxScorerSupplier(std::vector<std::unique_ptr<ScorerSupplier>> suppliers, float tie_breaker_multiplier,
                                 ScoreMode score_mode)
            : suppliers_(std::move(suppliers)),
              tie_breaker_multiplier_(tie_breaker_multiplier),
              score_mode_(score_mode) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t lead_cost, const LeafReaderContext &context,
                                                      const IndexSearcher &searcher) override {
        std::vector<std::unique_ptr<Scorer>> scorers;
        scorers.reserve(suppliers_.size());
        for (auto &supplier : suppliers_) {
            auto scorer = supplier->get(lead_cost, context, searcher);
            if (!scorer.has_value()) {
                return std::unexpected(std::move(scorer.error()));
            }
            if (scorer.value() == nullptr) {
                return std::unique_ptr<Scorer>{};
            }
            scorers.push_back(std::move(scorer.value()));
        }
        if (scorers.size() == 1) {
            return std::move(scorers.front());
        }
        auto disjunction = DisjunctionMaxScorer::create(tie_breaker_multiplier_, std::move(scorers), score_mode_);
        if (!disjunction.has_value()) {
            return std::unexpected(std::move(disjunction.error()));
        }
        return std::unique_ptr<Scorer>(std::move(disjunction.value()));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext &context, const IndexSearcher &searcher) override {
        if (cost_ == -1) {
            int64_t cost = 0;
            for (auto &supplier : suppliers_) {
                auto sub_cost = supplier->cost(context, searcher);
                if (!sub_cost.has_value()) {
                    return std::unexpected(std::move(sub_cost.error()));
                }
                cost += sub_cost.value();
            }
            cost_ = cost;
        }
        return cost_;
    }

    [[nodiscard]] VoidResult set_top_level_scoring_clause() override {
        if (tie_breaker_multiplier_ != 0.0F) {
            // Only a plain maximum propagates the competitive score down.
            return {};
        }
        for (auto &supplier : suppliers_) {
            if (auto set = supplier->set_top_level_scoring_clause(); !set.has_value()) {
                return set;
            }
        }
        return {};
    }

private:
    std::vector<std::unique_ptr<ScorerSupplier>> suppliers_;
    float tie_breaker_multiplier_;
    ScoreMode score_mode_;
    int64_t cost_{-1};
};

/// The weight of a [`DisjunctionMaxQuery`].
class DisjunctionMaxWeight final : public Weight {
public:
    DisjunctionMaxWeight(const Query &parent_query, std::vector<std::unique_ptr<Weight>> weights,
                         float tie_breaker_multiplier, ScoreMode score_mode)
            : parent_query_(&parent_query),
              weights_(std::move(weights)),
              tie_breaker_multiplier_(tie_breaker_multiplier),
              score_mode_(score_mode) {}

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override {
        if (weights_.size() > kCacheableDisjunctLimit) {
            // Do not encourage large disjunctions as a way around the clause
            // limit of the constant score rewrite.
            return false;
        }
        for (const auto &weight : weights_) {
            auto cacheable = weight->is_cacheable(context);
            if (!cacheable.has_value()) {
                return std::unexpected(std::move(cacheable.error()));
            }
            if (!cacheable.value()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override {
        bool matched = false;
        double maximum = 0.0;
        double other_sum = 0.0;
        std::vector<Explanation> matched_details;
        std::vector<Explanation> unmatched_details;

        for (const auto &weight : weights_) {
            auto explanation = weight->explain(context, doc, searcher);
            if (!explanation.has_value()) {
                return std::unexpected(std::move(explanation.error()));
            }
            if (explanation.value().is_match()) {
                matched = true;
                const double score = number_as_f64(explanation.value().value());
                matched_details.push_back(std::move(explanation.value()));
                if (score >= maximum) {
                    other_sum += maximum;
                    maximum = score;
                } else {
                    other_sum += score;
                }
            } else if (!matched) {
                unmatched_details.push_back(std::move(explanation.value()));
            }
        }

        if (!matched) {
            return Explanation::no_match("No matching clause", std::move(unmatched_details));
        }
        const float score = static_cast<float>(maximum + other_sum * static_cast<double>(tie_breaker_multiplier_));
        const std::string description = tie_breaker_multiplier_ == 0.0F
                                                ? std::string("max of:")
                                                : std::format("max plus {} times others of:", tie_breaker_multiplier_);
        return Explanation::match_(score, description, std::move(matched_details));
    }

    [[nodiscard]] const Query &get_query() const override { return *parent_query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher &searcher) override {
        std::vector<std::unique_ptr<ScorerSupplier>> suppliers;
        suppliers.reserve(weights_.size());
        for (auto &weight : weights_) {
            auto supplier = weight->scorer_supplier(context, searcher);
            if (!supplier.has_value()) {
                return std::unexpected(std::move(supplier.error()));
            }
            if (supplier.value() != nullptr) {
                suppliers.push_back(std::move(supplier.value()));
            }
        }
        if (suppliers.empty()) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        if (suppliers.size() == 1) {
            return std::move(suppliers.front());
        }
        return std::unique_ptr<ScorerSupplier>(
                new DisjunctionMaxScorerSupplier(std::move(suppliers), tie_breaker_multiplier_, score_mode_));
    }

private:
    const Query *parent_query_;
    std::vector<std::unique_ptr<Weight>> weights_;
    float tie_breaker_multiplier_;
    ScoreMode score_mode_;
};

} // namespace

DisjunctionMaxQuery::DisjunctionMaxQuery(std::vector<std::shared_ptr<Query>> disjuncts, float tie_breaker_multiplier)
        : disjuncts_(std::move(disjuncts)), tie_breaker_multiplier_(tie_breaker_multiplier) {}

Result<std::shared_ptr<DisjunctionMaxQuery>>
DisjunctionMaxQuery::create(std::vector<std::shared_ptr<Query>> disjuncts, float tie_breaker_multiplier) {
    // NaN passes neither comparison, which is what the reference implementation
    // relies on as well.
    if (tie_breaker_multiplier < 0.0F || tie_breaker_multiplier > 1.0F) {
        return err(LuceneError::illegal_argument("tie_breaker_multiplier must be in [0, 1]"));
    }
    for (const auto &disjunct : disjuncts) {
        if (disjunct == nullptr) {
            return err(LuceneError::illegal_argument("a disjunction max query needs non-null disjuncts"));
        }
    }
    std::shared_ptr<DisjunctionMaxQuery> query(
            new DisjunctionMaxQuery(std::move(disjuncts), tie_breaker_multiplier));
    return query;
}

Result<std::unique_ptr<Weight>> DisjunctionMaxQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const {
    std::vector<std::unique_ptr<Weight>> weights;
    weights.reserve(disjuncts_.size());
    for (const auto &disjunct : disjuncts_) {
        auto weight = disjunct->create_weight(searcher, score_mode, boost);
        if (!weight.has_value()) {
            return std::unexpected(std::move(weight.error()));
        }
        weights.push_back(std::move(weight.value()));
    }
    std::unique_ptr<Weight> weight(
            new DisjunctionMaxWeight(*this, std::move(weights), tie_breaker_multiplier_, score_mode));
    return weight;
}

Result<std::shared_ptr<Query>> DisjunctionMaxQuery::rewrite(const IndexSearcher &searcher) const {
    if (disjuncts_.empty()) {
        return std::shared_ptr<Query>(new MatchNoDocsQuery("empty DisjunctionMaxQuery"));
    }
    if (disjuncts_.size() == 1) {
        return disjuncts_.front();
    }
    if (tie_breaker_multiplier_ == 1.0F) {
        // Every disjunct counts fully, which is a plain boolean query.
        BooleanQuery::Builder builder;
        for (const auto &disjunct : disjuncts_) {
            if (auto added = builder.add(disjunct, Occur::Should); !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
        }
        auto built = builder.build();
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        return std::shared_ptr<Query>(new BooleanQuery(std::move(built.value())));
    }

    bool actually_rewritten = false;
    std::vector<std::shared_ptr<Query>> rewritten_disjuncts;
    rewritten_disjuncts.reserve(disjuncts_.size());
    for (const auto &disjunct : disjuncts_) {
        auto rewritten = disjunct->rewrite(searcher);
        if (!rewritten.has_value()) {
            return std::unexpected(std::move(rewritten.error()));
        }
        if (rewritten.value() != nullptr) {
            actually_rewritten = true;
            rewritten_disjuncts.push_back(std::move(rewritten.value()));
        } else {
            rewritten_disjuncts.push_back(disjunct);
        }
    }
    if (!actually_rewritten) {
        // Nothing changed: a null rewrite means "already in final form".
        return std::shared_ptr<Query>{};
    }
    auto query = create(std::move(rewritten_disjuncts), tie_breaker_multiplier_);
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::static_pointer_cast<Query>(query.value());
}

VoidResult DisjunctionMaxQuery::visit(QueryVisitor &visitor) const {
    // **Deviation:** the reference implementation visits through a sub visitor for
    // the `SHOULD` occurrence; the port has no `get_sub_visitor` yet.
    for (const auto &disjunct : disjuncts_) {
        if (auto visited = disjunct->visit(visitor); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

Result<std::string> DisjunctionMaxQuery::to_string(std::string_view field) const {
    std::string result = "(";
    for (size_t i = 0; i < disjuncts_.size(); ++i) {
        if (i > 0) {
            result += " | ";
        }
        auto text = disjuncts_[i]->to_string(field);
        if (!text.has_value()) {
            return std::unexpected(std::move(text.error()));
        }
        if (dynamic_cast<const BooleanQuery *>(disjuncts_[i].get()) != nullptr) {
            result.push_back('(');
            result += text.value();
            result.push_back(')');
        } else {
            result += text.value();
        }
    }
    result.push_back(')');
    if (tie_breaker_multiplier_ != 0.0F) {
        result.push_back('~');
        result += std::format("{:.1f}", tie_breaker_multiplier_);
    }
    return result;
}

bool DisjunctionMaxQuery::equals(const Query &other) const {
    const auto *disjunction = dynamic_cast<const DisjunctionMaxQuery *>(&other);
    if (disjunction == nullptr) {
        return false;
    }
    if (disjuncts_.size() != disjunction->disjuncts_.size()) {
        return false;
    }
    // The comparison is order insensitive, like the reference implementation's
    // multiset comparison.
    if (std::bit_cast<uint32_t>(tie_breaker_multiplier_) !=
        std::bit_cast<uint32_t>(disjunction->tie_breaker_multiplier_)) {
        return false;
    }
    std::vector<bool> used(disjunction->disjuncts_.size(), false);
    for (const auto &disjunct : disjuncts_) {
        bool found = false;
        for (size_t i = 0; i < disjunction->disjuncts_.size(); ++i) {
            if (used[i]) {
                continue;
            }
            if (disjunct->equals(*disjunction->disjuncts_[i])) {
                used[i] = true;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

size_t DisjunctionMaxQuery::hash() const {
    size_t result = static_cast<size_t>(std::bit_cast<uint32_t>(tie_breaker_multiplier_));
    // Order insensitive, matching the equality above.
    size_t sum = 0;
    for (const auto &disjunct : disjuncts_) {
        sum += disjunct->hash();
    }
    result = result * 31 + sum;
    result = result * 31 + disjuncts_.size();
    return result;
}

} // namespace lucene
