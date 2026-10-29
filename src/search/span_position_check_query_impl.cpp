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

module lucene_core:span_position_check_query.impl;

import :span_position_check_query;

import :error;
import :filter_spans;
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

/// Runs the wrapped spans through the query's position check.
class PositionCheckSpans final : public FilterSpans {
public:
    PositionCheckSpans(std::unique_ptr<Spans> match, const SpanPositionCheckQuery &query)
        : FilterSpans(std::move(match)), query_(&query) {}

protected:
    [[nodiscard]] Result<SpanAcceptStatus> accept(Spans &candidate) override {
        return query_->accept_position(candidate);
    }

private:
    const SpanPositionCheckQuery *query_;
};

} // namespace

SpanPositionCheckWeight::SpanPositionCheckWeight(const SpanPositionCheckQuery &query,
                                                 std::unique_ptr<SpanWeight> match_weight,
                                                 const IndexSearcher &searcher, TermStatesMap term_states, float boost)
    : SpanWeight(query, searcher, std::move(term_states), boost), match_weight_(std::move(match_weight)) {}

VoidResult SpanPositionCheckWeight::extract_term_states(TermStatesMap &contexts) const {
    return match_weight_->extract_term_states(contexts);
}

Result<std::unique_ptr<Spans>> SpanPositionCheckWeight::get_spans(const LeafReaderContext &context,
                                                                 SpanPostings required) {
    auto match_spans = match_weight_->get_spans(context, required);
    if (!match_spans.has_value()) {
        return std::unexpected(std::move(match_spans.error()));
    }
    if (match_spans.value() == nullptr) {
        return std::unique_ptr<Spans>{};
    }
    const auto *query = dynamic_cast<const SpanPositionCheckQuery *>(query_);
    if (query == nullptr) {
        return err(LuceneError::illegal_state("a position check weight needs a position check query"));
    }
    return std::unique_ptr<Spans>(
            std::make_unique<PositionCheckSpans>(std::move(match_spans.value()), *query));
}

Result<bool> SpanPositionCheckWeight::is_cacheable(const LeafReaderContext &context) const {
    return match_weight_->is_cacheable(context);
}

SpanPositionCheckQuery::SpanPositionCheckQuery(std::shared_ptr<SpanQuery> match) : match_(std::move(match)) {
    debug_check(match_ != nullptr, "a position check query needs a wrapped query");
}

Result<std::unique_ptr<Weight>> SpanPositionCheckQuery::create_weight(const IndexSearcher &searcher,
                                                                     ScoreMode score_mode, float boost) const {
    const bool needs_scores = score_mode_needs_scores(score_mode);
    auto match_weight = match_->create_weight(searcher, score_mode, boost);
    if (!match_weight.has_value()) {
        return std::unexpected(std::move(match_weight.error()));
    }
    auto *span_weight = dynamic_cast<SpanWeight *>(match_weight.value().get());
    if (span_weight == nullptr) {
        return err(LuceneError::illegal_argument("a position check query must wrap a span query"));
    }
    match_weight.value().release();

    TermStatesMap term_states;
    if (needs_scores) {
        if (VoidResult extracted = span_weight->extract_term_states(term_states); !extracted.has_value()) {
            return std::unexpected(std::move(extracted.error()));
        }
    }
    return std::unique_ptr<Weight>(std::make_unique<SpanPositionCheckWeight>(
            *this, std::unique_ptr<SpanWeight>(span_weight), searcher, std::move(term_states), boost));
}

Result<std::shared_ptr<Query>> SpanPositionCheckQuery::rewrite(const IndexSearcher &searcher) const {
    auto rewritten = match_->rewrite(searcher);
    if (!rewritten.has_value()) {
        return std::unexpected(std::move(rewritten.error()));
    }
    if (rewritten.value() == nullptr) {
        return std::shared_ptr<Query>{};
    }
    auto *span_match = dynamic_cast<SpanQuery *>(rewritten.value().get());
    if (span_match == nullptr) {
        return err(LuceneError::illegal_argument("a position check rewrite produced a non span query"));
    }
    return std::shared_ptr<Query>(with_match(std::shared_ptr<SpanQuery>(rewritten.value(), span_match)));
}

VoidResult SpanPositionCheckQuery::visit(QueryVisitor &visitor) const {
    if (!visitor.accept_field(get_field())) {
        return {};
    }
    DefaultQueryVisitor sub_visitor = visitor.get_sub_visitor(Occur::Must, *this);
    return match_->visit(sub_visitor);
}

bool SpanPositionCheckQuery::equals(const Query &other) const {
    if (typeid(*this) != typeid(other)) {
        return false;
    }
    const auto &query = dynamic_cast<const SpanPositionCheckQuery &>(other);
    return match_->equals(*query.match_);
}

size_t SpanPositionCheckQuery::hash() const { return 0x7FEB352DU ^ match_->hash(); }

} // namespace lucene
