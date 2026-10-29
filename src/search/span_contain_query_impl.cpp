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

module lucene_core:span_contain_query.impl;

import :span_contain_query;

import :conjunction_spans;
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

ContainSpans::ContainSpans(std::vector<std::unique_ptr<Spans>> sub_spans, size_t lead, bool containing)
    : ConjunctionSpans(std::move(sub_spans)), lead_(lead), containing_(containing) {
    debug_check(sub_spans_.size() == 2, "a contain spans needs exactly two operands");
    debug_check(lead_ < 2, "the lead operand must be one of the two");
}

int32_t ContainSpans::start_position() const {
    if (at_first_in_current_doc_) {
        return -1;
    }
    return one_exhausted_in_current_doc_ ? NO_MORE_POSITIONS : lead().start_position();
}

int32_t ContainSpans::end_position() const {
    if (at_first_in_current_doc_) {
        return -1;
    }
    return one_exhausted_in_current_doc_ ? NO_MORE_POSITIONS : lead().end_position();
}

VoidResult ContainSpans::collect(SpanCollector &collector) {
    if (VoidResult collected = big().collect(collector); !collected.has_value()) {
        return collected;
    }
    return little().collect(collector);
}

Result<bool> ContainSpans::two_phase_current_doc_matches() {
    one_exhausted_in_current_doc_ = false;

    if (containing_) {
        while (true) {
            auto big_next = big().next_start_position();
            if (!big_next.has_value()) {
                return std::unexpected(std::move(big_next.error()));
            }
            if (big_next.value() == NO_MORE_POSITIONS) {
                break;
            }
            while (little().start_position() < big().start_position()) {
                auto little_next = little().next_start_position();
                if (!little_next.has_value()) {
                    return std::unexpected(std::move(little_next.error()));
                }
                if (little_next.value() == NO_MORE_POSITIONS) {
                    one_exhausted_in_current_doc_ = true;
                    return false;
                }
            }
            if (big().end_position() >= little().end_position()) {
                at_first_in_current_doc_ = true;
                return true;
            }
        }
        one_exhausted_in_current_doc_ = true;
        return false;
    }

    while (true) {
        auto little_next = little().next_start_position();
        if (!little_next.has_value()) {
            return std::unexpected(std::move(little_next.error()));
        }
        if (little_next.value() == NO_MORE_POSITIONS) {
            break;
        }
        while (big().end_position() < little().end_position()) {
            auto big_next = big().next_start_position();
            if (!big_next.has_value()) {
                return std::unexpected(std::move(big_next.error()));
            }
            if (big_next.value() == NO_MORE_POSITIONS) {
                one_exhausted_in_current_doc_ = true;
                return false;
            }
        }
        if (big().start_position() <= little().start_position()) {
            at_first_in_current_doc_ = true;
            return true;
        }
    }
    one_exhausted_in_current_doc_ = true;
    return false;
}

Result<int32_t> ContainSpans::next_start_position() {
    if (at_first_in_current_doc_) {
        at_first_in_current_doc_ = false;
        return lead().start_position();
    }

    if (containing_) {
        while (true) {
            auto big_next = big().next_start_position();
            if (!big_next.has_value()) {
                return std::unexpected(std::move(big_next.error()));
            }
            if (big_next.value() == NO_MORE_POSITIONS) {
                break;
            }
            while (little().start_position() < big().start_position()) {
                auto little_next = little().next_start_position();
                if (!little_next.has_value()) {
                    return std::unexpected(std::move(little_next.error()));
                }
                if (little_next.value() == NO_MORE_POSITIONS) {
                    one_exhausted_in_current_doc_ = true;
                    return NO_MORE_POSITIONS;
                }
            }
            if (big().end_position() >= little().end_position()) {
                return big().start_position();
            }
        }
        one_exhausted_in_current_doc_ = true;
        return NO_MORE_POSITIONS;
    }

    while (true) {
        auto little_next = little().next_start_position();
        if (!little_next.has_value()) {
            return std::unexpected(std::move(little_next.error()));
        }
        if (little_next.value() == NO_MORE_POSITIONS) {
            break;
        }
        while (big().end_position() < little().end_position()) {
            auto big_next = big().next_start_position();
            if (!big_next.has_value()) {
                return std::unexpected(std::move(big_next.error()));
            }
            if (big_next.value() == NO_MORE_POSITIONS) {
                one_exhausted_in_current_doc_ = true;
                return NO_MORE_POSITIONS;
            }
        }
        if (big().start_position() <= little().start_position()) {
            return little().start_position();
        }
    }
    one_exhausted_in_current_doc_ = true;
    return NO_MORE_POSITIONS;
}

SpanContainWeight::SpanContainWeight(const SpanContainQuery &query, std::unique_ptr<SpanWeight> big_weight,
                                     std::unique_ptr<SpanWeight> little_weight, const IndexSearcher &searcher,
                                     TermStatesMap term_states, float boost)
    : SpanWeight(query, searcher, std::move(term_states), boost),
      big_weight_(std::move(big_weight)),
      little_weight_(std::move(little_weight)) {}

VoidResult SpanContainWeight::extract_term_states(TermStatesMap &contexts) const {
    if (VoidResult extracted = big_weight_->extract_term_states(contexts); !extracted.has_value()) {
        return extracted;
    }
    return little_weight_->extract_term_states(contexts);
}

Result<bool> SpanContainWeight::is_cacheable(const LeafReaderContext &context) const {
    auto big_cacheable = big_weight_->is_cacheable(context);
    if (!big_cacheable.has_value()) {
        return std::unexpected(std::move(big_cacheable.error()));
    }
    if (!big_cacheable.value()) {
        return false;
    }
    return little_weight_->is_cacheable(context);
}

Result<std::vector<std::unique_ptr<Spans>>> SpanContainWeight::prepare_conjunction(const LeafReaderContext &context,
                                                                                  SpanPostings required) {
    std::vector<std::unique_ptr<Spans>> operands;
    auto big_spans = big_weight_->get_spans(context, required);
    if (!big_spans.has_value()) {
        return std::unexpected(std::move(big_spans.error()));
    }
    if (big_spans.value() == nullptr) {
        return operands;
    }
    auto little_spans = little_weight_->get_spans(context, required);
    if (!little_spans.has_value()) {
        return std::unexpected(std::move(little_spans.error()));
    }
    if (little_spans.value() == nullptr) {
        return operands;
    }
    operands.push_back(std::move(big_spans.value()));
    operands.push_back(std::move(little_spans.value()));
    return operands;
}

SpanContainQuery::SpanContainQuery(std::shared_ptr<SpanQuery> big, std::shared_ptr<SpanQuery> little,
                                   uint32_t class_hash)
    : big_(std::move(big)), little_(std::move(little)), class_hash_(class_hash) {}

Result<std::unique_ptr<Weight>> SpanContainQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const {
    const bool needs_scores = score_mode_needs_scores(score_mode);

    auto big_weight = big_->create_weight(searcher, score_mode, boost);
    if (!big_weight.has_value()) {
        return std::unexpected(std::move(big_weight.error()));
    }
    auto *big_span_weight = dynamic_cast<SpanWeight *>(big_weight.value().get());
    if (big_span_weight == nullptr) {
        return err(LuceneError::illegal_argument("a span contain operand must produce a span weight"));
    }
    big_weight.value().release();

    auto little_weight = little_->create_weight(searcher, score_mode, boost);
    if (!little_weight.has_value()) {
        return std::unexpected(std::move(little_weight.error()));
    }
    auto *little_span_weight = dynamic_cast<SpanWeight *>(little_weight.value().get());
    if (little_span_weight == nullptr) {
        return err(LuceneError::illegal_argument("a span contain operand must produce a span weight"));
    }
    little_weight.value().release();

    std::unique_ptr<SpanWeight> big_owner(big_span_weight);
    std::unique_ptr<SpanWeight> little_owner(little_span_weight);

    TermStatesMap term_states;
    if (needs_scores) {
        if (VoidResult extracted = big_owner->extract_term_states(term_states); !extracted.has_value()) {
            return std::unexpected(std::move(extracted.error()));
        }
        if (VoidResult extracted = little_owner->extract_term_states(term_states); !extracted.has_value()) {
            return std::unexpected(std::move(extracted.error()));
        }
    }
    return std::unique_ptr<Weight>(build_weight(std::move(big_owner), std::move(little_owner), searcher,
                                                std::move(term_states), boost));
}

Result<std::shared_ptr<Query>> SpanContainQuery::rewrite(const IndexSearcher &searcher) const {
    auto rewritten_big = big_->rewrite(searcher);
    if (!rewritten_big.has_value()) {
        return std::unexpected(std::move(rewritten_big.error()));
    }
    auto rewritten_little = little_->rewrite(searcher);
    if (!rewritten_little.has_value()) {
        return std::unexpected(std::move(rewritten_little.error()));
    }
    if (rewritten_big.value() == nullptr && rewritten_little.value() == nullptr) {
        return std::shared_ptr<Query>{};
    }

    std::shared_ptr<Query> big = rewritten_big.value() != nullptr ? rewritten_big.value() : big_;
    std::shared_ptr<Query> little = rewritten_little.value() != nullptr ? rewritten_little.value() : little_;
    auto *big_span = dynamic_cast<SpanQuery *>(big.get());
    auto *little_span = dynamic_cast<SpanQuery *>(little.get());
    if (big_span == nullptr || little_span == nullptr) {
        return err(LuceneError::illegal_argument("a span contain rewrite produced a non span query"));
    }
    return with_parts(std::shared_ptr<SpanQuery>(big, big_span), std::shared_ptr<SpanQuery>(little, little_span));
}

VoidResult SpanContainQuery::visit(QueryVisitor &visitor) const {
    if (!visitor.accept_field(get_field())) {
        return {};
    }
    DefaultQueryVisitor sub_visitor = visitor.get_sub_visitor(Occur::Must, *this);
    if (VoidResult visited = big_->visit(sub_visitor); !visited.has_value()) {
        return visited;
    }
    return little_->visit(sub_visitor);
}

bool SpanContainQuery::equals(const Query &other) const {
    if (typeid(*this) != typeid(other)) {
        return false;
    }
    const auto &query = dynamic_cast<const SpanContainQuery &>(other);
    return big_->equals(*query.big_) && little_->equals(*query.little_);
}

size_t SpanContainQuery::hash() const {
    size_t result = class_hash_;
    result = result * 31 + big_->hash();
    result = result * 31 + little_->hash();
    return result;
}

Result<std::string> SpanContainQuery::to_string_with_name(std::string_view field, std::string_view name) const {
    auto big_text = big_->to_string(field);
    if (!big_text.has_value()) {
        return std::unexpected(std::move(big_text.error()));
    }
    auto little_text = little_->to_string(field);
    if (!little_text.has_value()) {
        return std::unexpected(std::move(little_text.error()));
    }
    return std::string(name) + "(" + big_text.value() + ", " + little_text.value() + ")";
}

} // namespace lucene
