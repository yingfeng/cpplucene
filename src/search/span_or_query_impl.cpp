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

module lucene_core:span_or_query.impl;

import :span_or_query;

import :doc_id_set_iterator;
import :error;
import :index_searcher;
import :leaf_reader_context;
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

namespace {

constexpr size_t NO_CLAUSE = std::numeric_limits<size_t>::max();

/// The merged spans of a [`SpanOrQuery`]: documents are the union of the clauses'
/// documents, and the positions of all clauses that match the current document
/// are walked in increasing `(start, end)` order.
class SpanOrSpans final : public Spans {
public:
    explicit SpanOrSpans(std::vector<std::unique_ptr<Spans>> sub_spans) : sub_spans_(std::move(sub_spans)) {
        debug_check(!sub_spans_.empty(), "a span or needs at least one sub spans");
    }

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        const int32_t target = doc_ == -1 ? 0 : doc_ + 1;
        return seek(target);
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return seek(target); }

    [[nodiscard]] Result<int64_t> cost() const override {
        int64_t total = 0;
        for (const std::unique_ptr<Spans> &span : sub_spans_) {
            auto sub_cost = span->cost();
            if (!sub_cost.has_value()) {
                return std::unexpected(std::move(sub_cost.error()));
            }
            total += sub_cost.value();
        }
        return total;
    }

    [[nodiscard]] Result<int32_t> next_start_position() override {
        if (positioned_ && current_ != NO_CLAUSE) {
            // Only the clause that produced the current position has to move.
            auto advanced = sub_spans_[current_]->next_start_position();
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
        }
        positioned_ = true;

        size_t best = NO_CLAUSE;
        int32_t best_start = 0;
        int32_t best_end = 0;
        for (size_t i = 0; i < sub_spans_.size(); ++i) {
            const Spans &candidate = *sub_spans_[i];
            const int32_t start = candidate.start_position();
            if (start < 0 || start == NO_MORE_POSITIONS) {
                continue;
            }
            const int32_t end = candidate.end_position();
            if (best == NO_CLAUSE || start < best_start || (start == best_start && end < best_end)) {
                best = i;
                best_start = start;
                best_end = end;
            }
        }

        current_ = best;
        if (best == NO_CLAUSE) {
            current_start_ = NO_MORE_POSITIONS;
            current_end_ = NO_MORE_POSITIONS;
            return NO_MORE_POSITIONS;
        }
        current_start_ = best_start;
        current_end_ = best_end;
        return current_start_;
    }

    [[nodiscard]] int32_t start_position() const override { return current_start_; }
    [[nodiscard]] int32_t end_position() const override { return current_end_; }

    [[nodiscard]] int32_t width() const override {
        if (current_ == NO_CLAUSE) {
            return 0;
        }
        return sub_spans_[current_]->width();
    }

    [[nodiscard]] VoidResult collect(SpanCollector &collector) override {
        if (current_ == NO_CLAUSE) {
            return {};
        }
        return sub_spans_[current_]->collect(collector);
    }

    [[nodiscard]] float positions_cost() const override {
        float total = 0.0F;
        for (const std::unique_ptr<Spans> &span : sub_spans_) {
            total += span->positions_cost();
        }
        return total;
    }

private:
    /// Moves every clause to the smallest document at or after `target`, then
    /// places each clause on its first position of that document.
    [[nodiscard]] Result<int32_t> seek(int32_t target) {
        int32_t min_doc = NO_MORE_DOCS;
        for (const std::unique_ptr<Spans> &span : sub_spans_) {
            if (span->doc_id() < target) {
                auto advanced = span->advance(target);
                if (!advanced.has_value()) {
                    return std::unexpected(std::move(advanced.error()));
                }
            }
            min_doc = std::min(min_doc, span->doc_id());
        }

        doc_ = min_doc;
        positioned_ = false;
        current_ = NO_CLAUSE;
        current_start_ = -1;
        current_end_ = -1;
        if (doc_ == NO_MORE_DOCS) {
            return doc_;
        }

        for (const std::unique_ptr<Spans> &span : sub_spans_) {
            if (span->doc_id() != doc_) {
                continue;
            }
            // A spans always has at least one position in a document it reached,
            // but a clause whose document is further along must not be touched.
            while (true) {
                auto position = span->next_start_position();
                if (!position.has_value()) {
                    return std::unexpected(std::move(position.error()));
                }
                if (position.value() != NO_MORE_POSITIONS) {
                    break;
                }
            }
        }
        return doc_;
    }

    std::vector<std::unique_ptr<Spans>> sub_spans_;
    int32_t doc_{-1};
    int32_t current_start_{-1};
    int32_t current_end_{-1};
    size_t current_{NO_CLAUSE};
    bool positioned_{false};
};

} // namespace

SpanOrWeight::SpanOrWeight(const SpanOrQuery &query, std::vector<std::unique_ptr<SpanWeight>> sub_weights,
                           const IndexSearcher &searcher, TermStatesMap term_states, float boost)
    : SpanWeight(query, searcher, std::move(term_states), boost), sub_weights_(std::move(sub_weights)) {}

VoidResult SpanOrWeight::extract_term_states(TermStatesMap &contexts) const {
    for (const std::unique_ptr<SpanWeight> &sub_weight : sub_weights_) {
        if (VoidResult extracted = sub_weight->extract_term_states(contexts); !extracted.has_value()) {
            return extracted;
        }
    }
    return {};
}

Result<std::unique_ptr<Spans>> SpanOrWeight::get_spans(const LeafReaderContext &context, SpanPostings required) {
    std::vector<std::unique_ptr<Spans>> sub_spans;
    sub_spans.reserve(sub_weights_.size());
    for (const std::unique_ptr<SpanWeight> &sub_weight : sub_weights_) {
        auto spans = sub_weight->get_spans(context, required);
        if (!spans.has_value()) {
            return std::unexpected(std::move(spans.error()));
        }
        // A clause without spans in this leaf is skipped, not fatal: the others
        // can still match.
        if (spans.value() != nullptr) {
            sub_spans.push_back(std::move(spans.value()));
        }
    }
    if (sub_spans.empty()) {
        return std::unique_ptr<Spans>{};
    }
    return std::unique_ptr<Spans>(std::make_unique<SpanOrSpans>(std::move(sub_spans)));
}

SpanOrQuery::SpanOrQuery(std::vector<std::shared_ptr<SpanQuery>> clauses) : clauses_(std::move(clauses)) {}

Result<SpanOrQuery> SpanOrQuery::create(std::vector<std::shared_ptr<SpanQuery>> clauses) {
    if (clauses.empty()) {
        return err(LuceneError::illegal_argument("a span or query needs at least one clause"));
    }
    const std::string_view field = clauses.front() != nullptr ? clauses.front()->get_field() : std::string_view{};
    for (const std::shared_ptr<SpanQuery> &clause : clauses) {
        if (clause == nullptr) {
            return err(LuceneError::illegal_argument("a span or clause must not be null"));
        }
        if (clause->get_field() != field) {
            return err(LuceneError::illegal_argument("clauses must have the same field"));
        }
    }
    return SpanOrQuery(std::move(clauses));
}

Result<std::unique_ptr<Weight>> SpanOrQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                           float boost) const {
    const bool needs_scores = score_mode_needs_scores(score_mode);
    std::vector<std::unique_ptr<SpanWeight>> sub_weights;
    sub_weights.reserve(clauses_.size());
    for (const std::shared_ptr<SpanQuery> &clause : clauses_) {
        auto weight = clause->create_weight(searcher, score_mode, boost);
        if (!weight.has_value()) {
            return std::unexpected(std::move(weight.error()));
        }
        auto *span_weight = dynamic_cast<SpanWeight *>(weight.value().get());
        if (span_weight == nullptr) {
            return err(LuceneError::illegal_argument("span or clauses must produce span weights"));
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
            std::make_unique<SpanOrWeight>(*this, std::move(sub_weights), searcher, std::move(term_states), boost));
}

Result<std::shared_ptr<Query>> SpanOrQuery::rewrite(const IndexSearcher &searcher) const {
    std::vector<std::shared_ptr<SpanQuery>> rewritten_clauses;
    rewritten_clauses.reserve(clauses_.size());
    bool changed = false;
    for (const std::shared_ptr<SpanQuery> &clause : clauses_) {
        auto rewritten = clause->rewrite(searcher);
        if (!rewritten.has_value()) {
            return std::unexpected(std::move(rewritten.error()));
        }
        std::shared_ptr<Query> replacement = rewritten.value() != nullptr ? rewritten.value() : clause;
        const bool same = replacement.get() == clause.get();
        changed = changed || !same;
        auto *span_clause = dynamic_cast<SpanQuery *>(replacement.get());
        if (span_clause == nullptr) {
            return err(LuceneError::illegal_argument("a span or rewrite produced a non span query"));
        }
        rewritten_clauses.emplace_back(replacement, span_clause);
    }
    if (!changed) {
        return std::shared_ptr<Query>{};
    }
    auto rebuilt = SpanOrQuery::create(std::move(rewritten_clauses));
    if (!rebuilt.has_value()) {
        return std::unexpected(std::move(rebuilt.error()));
    }
    return std::make_shared<SpanOrQuery>(std::move(rebuilt.value()));
}

VoidResult SpanOrQuery::visit(QueryVisitor &visitor) const {
    if (!visitor.accept_field(get_field())) {
        return {};
    }
    DefaultQueryVisitor sub_visitor = visitor.get_sub_visitor(Occur::Should, *this);
    for (const std::shared_ptr<SpanQuery> &clause : clauses_) {
        if (VoidResult visited = clause->visit(sub_visitor); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

Result<std::string> SpanOrQuery::to_string(std::string_view field) const {
    std::string text = "spanOr([";
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
    text += "])";
    return text;
}

bool SpanOrQuery::equals(const Query &other) const {
    if (typeid(*this) != typeid(other)) {
        return false;
    }
    const auto &query = dynamic_cast<const SpanOrQuery &>(other);
    if (clauses_.size() != query.clauses_.size()) {
        return false;
    }
    for (size_t i = 0; i < clauses_.size(); ++i) {
        if (!clauses_[i]->equals(*query.clauses_[i])) {
            return false;
        }
    }
    return true;
}

size_t SpanOrQuery::hash() const {
    size_t result = 0x85EBCA6BU;
    for (const std::shared_ptr<SpanQuery> &clause : clauses_) {
        result = result * 31 + clause->hash();
    }
    return result;
}

} // namespace lucene
