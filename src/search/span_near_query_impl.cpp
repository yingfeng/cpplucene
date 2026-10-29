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

module lucene_core:span_near_query.impl;

import :span_near_query;

import :error;
import :index_searcher;
import :leaf_reader_context;
import :near_spans_ordered;
import :near_spans_unordered;
import :occur;
import :query;
import :query_visitor;
import :score_mode;
import :span_query;
import :span_weight;
import :spans;
import :types;
import :weight;

import std;

namespace lucene {

SpanNearWeight::SpanNearWeight(const SpanNearQuery &query, std::vector<std::unique_ptr<SpanWeight>> sub_weights,
                               const IndexSearcher &searcher, TermStatesMap term_states, float boost)
    : SpanWeight(query, searcher, std::move(term_states), boost), sub_weights_(std::move(sub_weights)) {}

VoidResult SpanNearWeight::extract_term_states(TermStatesMap &contexts) const {
    for (const std::unique_ptr<SpanWeight> &sub_weight : sub_weights_) {
        if (VoidResult extracted = sub_weight->extract_term_states(contexts); !extracted.has_value()) {
            return extracted;
        }
    }
    return {};
}

Result<std::unique_ptr<Spans>> SpanNearWeight::get_spans(const LeafReaderContext &context, SpanPostings required) {
    std::vector<std::unique_ptr<Spans>> sub_spans;
    sub_spans.reserve(sub_weights_.size());
    for (const std::unique_ptr<SpanWeight> &sub_weight : sub_weights_) {
        auto spans = sub_weight->get_spans(context, required);
        if (!spans.has_value()) {
            return std::unexpected(std::move(spans.error()));
        }
        if (spans.value() == nullptr) {
            // One clause has no spans in this leaf, so nothing can match.
            return std::unique_ptr<Spans>{};
        }
        sub_spans.push_back(std::move(spans.value()));
    }

    const auto *near_query = dynamic_cast<const SpanNearQuery *>(query_);
    if (near_query == nullptr) {
        return err(LuceneError::illegal_state("a span near weight needs a span near query"));
    }
    if (!near_query->in_order()) {
        return std::unique_ptr<Spans>(
                std::make_unique<NearSpansUnordered>(near_query->slop(), std::move(sub_spans)));
    }
    return std::unique_ptr<Spans>(std::make_unique<NearSpansOrdered>(near_query->slop(), std::move(sub_spans)));
}

SpanNearQuery::SpanNearQuery(std::vector<std::shared_ptr<SpanQuery>> clauses, int32_t slop, bool in_order)
    : clauses_(std::move(clauses)), slop_(slop), in_order_(in_order) {}

Result<SpanNearQuery> SpanNearQuery::create(std::vector<std::shared_ptr<SpanQuery>> clauses, int32_t slop,
                                            bool in_order) {
    if (clauses.empty()) {
        return err(LuceneError::illegal_argument("a span near query needs at least one clause"));
    }
    const std::string_view field = clauses.front() != nullptr ? clauses.front()->get_field() : std::string_view{};
    for (const std::shared_ptr<SpanQuery> &clause : clauses) {
        if (clause == nullptr) {
            return err(LuceneError::illegal_argument("a span near clause must not be null"));
        }
        if (clause->get_field() != field) {
            return err(LuceneError::illegal_argument("all span near clauses must be on the same field"));
        }
    }
    return SpanNearQuery(std::move(clauses), slop, in_order);
}

Result<std::unique_ptr<Weight>> SpanNearQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                             float boost) const {
    const bool needs_scores = score_mode_needs_scores(score_mode);
    std::vector<std::unique_ptr<SpanWeight>> sub_weights;
    sub_weights.reserve(clauses_.size());
    for (const std::shared_ptr<SpanQuery> &clause : clauses_) {
        auto weight = clause->create_weight(searcher, score_mode, 1.0F);
        if (!weight.has_value()) {
            return std::unexpected(std::move(weight.error()));
        }
        auto *span_weight = dynamic_cast<SpanWeight *>(weight.value().get());
        if (span_weight == nullptr) {
            return err(LuceneError::illegal_argument("span near clauses must produce span weights"));
        }
        weight.value().release();
        sub_weights.emplace_back(span_weight);
    }

    TermStatesMap term_states;
    if (needs_scores) {
        for (const std::unique_ptr<SpanWeight> &sub_weight : sub_weights) {
            if (VoidResult extracted = sub_weight->extract_term_states(term_states); !extracted.has_value()) {
                return std::unexpected(std::move(extracted.error()));
            }
        }
    }
    return std::unique_ptr<Weight>(
            std::make_unique<SpanNearWeight>(*this, std::move(sub_weights), searcher, std::move(term_states), boost));
}

Result<std::shared_ptr<Query>> SpanNearQuery::rewrite(const IndexSearcher &searcher) const {
    std::vector<std::shared_ptr<SpanQuery>> rewritten;
    rewritten.reserve(clauses_.size());
    bool changed = false;
    for (const std::shared_ptr<SpanQuery> &clause : clauses_) {
        auto clause_rewrite = clause->rewrite(searcher);
        if (!clause_rewrite.has_value()) {
            return std::unexpected(std::move(clause_rewrite.error()));
        }
        std::shared_ptr<Query> replacement = clause_rewrite.value() != nullptr ? clause_rewrite.value() : clause;
        changed = changed || replacement.get() != clause.get();
        auto *span_clause = dynamic_cast<SpanQuery *>(replacement.get());
        if (span_clause == nullptr) {
            return err(LuceneError::illegal_argument("a span near rewrite produced a non span query"));
        }
        rewritten.emplace_back(replacement, span_clause);
    }
    if (!changed) {
        return std::shared_ptr<Query>{};
    }
    auto rebuilt = SpanNearQuery::create(std::move(rewritten), slop_, in_order_);
    if (!rebuilt.has_value()) {
        return std::unexpected(std::move(rebuilt.error()));
    }
    return std::make_shared<SpanNearQuery>(std::move(rebuilt.value()));
}

VoidResult SpanNearQuery::visit(QueryVisitor &visitor) const {
    DefaultQueryVisitor sub_visitor = visitor.get_sub_visitor(Occur::Must, *this);
    for (const std::shared_ptr<SpanQuery> &clause : clauses_) {
        if (VoidResult visited = clause->visit(sub_visitor); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

Result<std::string> SpanNearQuery::to_string(std::string_view field) const {
    std::string text = "spanNear([";
    for (size_t i = 0; i < clauses_.size(); ++i) {
        auto clause_text = clauses_[i]->to_string(field);
        if (!clause_text.has_value()) {
            return std::unexpected(std::move(clause_text.error()));
        }
        if (i > 0) {
            text += ", ";
        }
        text += clause_text.value();
    }
    text += "], ";
    text += std::to_string(slop_);
    text += ", ";
    text += in_order_ ? "true" : "false";
    text += ")";
    return text;
}

bool SpanNearQuery::equals(const Query &other) const {
    if (typeid(*this) != typeid(other)) {
        return false;
    }
    const auto &query = dynamic_cast<const SpanNearQuery &>(other);
    if (slop_ != query.slop_ || in_order_ != query.in_order_ || clauses_.size() != query.clauses_.size()) {
        return false;
    }
    for (size_t i = 0; i < clauses_.size(); ++i) {
        if (!clauses_[i]->equals(*query.clauses_[i])) {
            return false;
        }
    }
    return true;
}

size_t SpanNearQuery::hash() const {
    size_t result = 0x9E3779B9U;
    for (const std::shared_ptr<SpanQuery> &clause : clauses_) {
        result = result * 31 + clause->hash();
    }
    result = result * 31 + static_cast<size_t>(slop_);
    result = result * 31 + (in_order_ ? 1U : 0U);
    return result;
}

} // namespace lucene
