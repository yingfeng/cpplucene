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

module lucene_core:span_not_query.impl;

import :span_not_query;

import :doc_id_set_iterator;
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

/// The filter described by [`SpanNotQuery`]: it keeps the include spans that a
/// nearby exclude span does not cover.
class SpanNotFilterSpans final : public FilterSpans {
public:
    SpanNotFilterSpans(std::unique_ptr<Spans> include, std::unique_ptr<Spans> exclude, int32_t pre, int32_t post)
        : FilterSpans(std::move(include)), exclude_(std::move(exclude)), pre_(pre), post_(post) {}

protected:
    [[nodiscard]] Result<SpanAcceptStatus> accept(Spans &candidate) override {
        const int32_t doc = candidate.doc_id();

        // Catch the exclusion up to the candidate's document.
        if (doc > exclude_->doc_id()) {
            auto advanced = exclude_->advance(doc);
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
        }
        if (doc != exclude_->doc_id()) {
            // The exclusion has no span in this document.
            return SpanAcceptStatus::kYes;
        }

        if (exclude_->start_position() == -1) {
            auto first = exclude_->next_start_position();
            if (!first.has_value()) {
                return std::unexpected(std::move(first.error()));
            }
        }

        // Skip the exclusion spans that end before the window opens.
        while (exclude_->end_position() <= candidate.start_position() - pre_) {
            auto next = exclude_->next_start_position();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            if (next.value() == NO_MORE_POSITIONS) {
                return SpanAcceptStatus::kYes;
            }
        }

        // `exclude.start - post >= candidate.end` means it starts after the window
        // closes, so the candidate survives.
        if (exclude_->start_position() - post_ >= candidate.end_position()) {
            return SpanAcceptStatus::kYes;
        }
        return SpanAcceptStatus::kNo;
    }

private:
    std::unique_ptr<Spans> exclude_;
    int32_t pre_;
    int32_t post_;
};

} // namespace

SpanNotWeight::SpanNotWeight(const SpanNotQuery &query, std::unique_ptr<SpanWeight> include_weight,
                             std::unique_ptr<SpanWeight> exclude_weight, const IndexSearcher &searcher,
                             TermStatesMap term_states, float boost)
    : SpanWeight(query, searcher, std::move(term_states), boost),
      include_weight_(std::move(include_weight)),
      exclude_weight_(std::move(exclude_weight)) {}

VoidResult SpanNotWeight::extract_term_states(TermStatesMap &contexts) const {
    // The exclusion never contributes to the score.
    return include_weight_->extract_term_states(contexts);
}

Result<std::unique_ptr<Spans>> SpanNotWeight::get_spans(const LeafReaderContext &context, SpanPostings required) {
    auto include_spans = include_weight_->get_spans(context, required);
    if (!include_spans.has_value()) {
        return std::unexpected(std::move(include_spans.error()));
    }
    if (include_spans.value() == nullptr) {
        return std::unique_ptr<Spans>{};
    }

    auto exclude_spans = exclude_weight_->get_spans(context, required);
    if (!exclude_spans.has_value()) {
        return std::unexpected(std::move(exclude_spans.error()));
    }
    if (exclude_spans.value() == nullptr) {
        // Nothing to exclude in this leaf.
        return std::move(include_spans.value());
    }

    const auto *not_query = dynamic_cast<const SpanNotQuery *>(query_);
    if (not_query == nullptr) {
        return err(LuceneError::illegal_state("a span not weight needs a span not query"));
    }
    return std::unique_ptr<Spans>(std::make_unique<SpanNotFilterSpans>(std::move(include_spans.value()),
                                                                      std::move(exclude_spans.value()),
                                                                      not_query->pre(), not_query->post()));
}

Result<bool> SpanNotWeight::is_cacheable(const LeafReaderContext &context) const {
    auto include_cacheable = include_weight_->is_cacheable(context);
    if (!include_cacheable.has_value()) {
        return std::unexpected(std::move(include_cacheable.error()));
    }
    if (!include_cacheable.value()) {
        return false;
    }
    return exclude_weight_->is_cacheable(context);
}

SpanNotQuery::SpanNotQuery(std::shared_ptr<SpanQuery> include, std::shared_ptr<SpanQuery> exclude, int32_t pre,
                           int32_t post)
    : include_(std::move(include)), exclude_(std::move(exclude)), pre_(pre), post_(post) {}

Result<SpanNotQuery> SpanNotQuery::create(std::shared_ptr<SpanQuery> include, std::shared_ptr<SpanQuery> exclude,
                                          int32_t pre, int32_t post) {
    if (include == nullptr || exclude == nullptr) {
        return err(LuceneError::illegal_argument("a span not query needs both an include and an exclude clause"));
    }
    if (include->get_field() != exclude->get_field()) {
        return err(LuceneError::illegal_argument("clauses must have the same field"));
    }
    return SpanNotQuery(std::move(include), std::move(exclude), pre, post);
}

Result<std::unique_ptr<Weight>> SpanNotQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                            float boost) const {
    const bool needs_scores = score_mode_needs_scores(score_mode);

    auto include_weight = include_->create_weight(searcher, score_mode, boost);
    if (!include_weight.has_value()) {
        return std::unexpected(std::move(include_weight.error()));
    }
    auto *include_span_weight = dynamic_cast<SpanWeight *>(include_weight.value().get());
    if (include_span_weight == nullptr) {
        return err(LuceneError::illegal_argument("a span not include clause must produce a span weight"));
    }
    include_weight.value().release();

    // The exclusion only has to know where its spans are, never how they score.
    auto exclude_weight = exclude_->create_weight(searcher, ScoreMode::CompleteNoScores, boost);
    if (!exclude_weight.has_value()) {
        return std::unexpected(std::move(exclude_weight.error()));
    }
    auto *exclude_span_weight = dynamic_cast<SpanWeight *>(exclude_weight.value().get());
    if (exclude_span_weight == nullptr) {
        return err(LuceneError::illegal_argument("a span not exclude clause must produce a span weight"));
    }
    exclude_weight.value().release();

    TermStatesMap term_states;
    if (needs_scores) {
        if (VoidResult extracted = include_span_weight->extract_term_states(term_states); !extracted.has_value()) {
            return std::unexpected(std::move(extracted.error()));
        }
    }
    return std::unique_ptr<Weight>(std::make_unique<SpanNotWeight>(*this, std::unique_ptr<SpanWeight>(include_span_weight),
                                                                  std::unique_ptr<SpanWeight>(exclude_span_weight),
                                                                  searcher, std::move(term_states), boost));
}

Result<std::shared_ptr<Query>> SpanNotQuery::rewrite(const IndexSearcher &searcher) const {
    auto rewritten_include = include_->rewrite(searcher);
    if (!rewritten_include.has_value()) {
        return std::unexpected(std::move(rewritten_include.error()));
    }
    auto rewritten_exclude = exclude_->rewrite(searcher);
    if (!rewritten_exclude.has_value()) {
        return std::unexpected(std::move(rewritten_exclude.error()));
    }
    if (rewritten_include.value() == nullptr && rewritten_exclude.value() == nullptr) {
        return std::shared_ptr<Query>{};
    }

    std::shared_ptr<Query> include = rewritten_include.value() != nullptr ? rewritten_include.value() : include_;
    std::shared_ptr<Query> exclude = rewritten_exclude.value() != nullptr ? rewritten_exclude.value() : exclude_;
    auto *include_span = dynamic_cast<SpanQuery *>(include.get());
    auto *exclude_span = dynamic_cast<SpanQuery *>(exclude.get());
    if (include_span == nullptr || exclude_span == nullptr) {
        return err(LuceneError::illegal_argument("a span not rewrite produced a non span query"));
    }
    auto rebuilt = SpanNotQuery::create(std::shared_ptr<SpanQuery>(include, include_span),
                                        std::shared_ptr<SpanQuery>(exclude, exclude_span), pre_, post_);
    if (!rebuilt.has_value()) {
        return std::unexpected(std::move(rebuilt.error()));
    }
    return std::make_shared<SpanNotQuery>(std::move(rebuilt.value()));
}

VoidResult SpanNotQuery::visit(QueryVisitor &visitor) const {
    if (!visitor.accept_field(get_field())) {
        return {};
    }
    DefaultQueryVisitor include_visitor = visitor.get_sub_visitor(Occur::Must, *this);
    if (VoidResult visited = include_->visit(include_visitor); !visited.has_value()) {
        return visited;
    }
    DefaultQueryVisitor exclude_visitor = visitor.get_sub_visitor(Occur::MustNot, *this);
    return exclude_->visit(exclude_visitor);
}

Result<std::string> SpanNotQuery::to_string(std::string_view field) const {
    auto include_text = include_->to_string(field);
    if (!include_text.has_value()) {
        return std::unexpected(std::move(include_text.error()));
    }
    auto exclude_text = exclude_->to_string(field);
    if (!exclude_text.has_value()) {
        return std::unexpected(std::move(exclude_text.error()));
    }
    return "spanNot(" + include_text.value() + ", " + exclude_text.value() + ", " + std::to_string(pre_) + ", " +
           std::to_string(post_) + ")";
}

bool SpanNotQuery::equals(const Query &other) const {
    if (typeid(*this) != typeid(other)) {
        return false;
    }
    const auto &query = dynamic_cast<const SpanNotQuery &>(other);
    return pre_ == query.pre_ && post_ == query.post_ && include_->equals(*query.include_) &&
           exclude_->equals(*query.exclude_);
}

size_t SpanNotQuery::hash() const {
    size_t result = 0x27D4EB2FU;
    result = result * 31 + include_->hash();
    result = result * 31 + exclude_->hash();
    result = result * 31 + static_cast<size_t>(pre_);
    result = result * 31 + static_cast<size_t>(post_);
    return result;
}

} // namespace lucene
