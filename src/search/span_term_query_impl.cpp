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

module lucene_core:span_term_query.impl;

import :span_term_query;

import :error;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :postings_enum;
import :query;
import :query_visitor;
import :score_mode;
import :span_query;
import :span_weight;
import :spans;
import :term;
import :term_spans;
import :term_states;
import :terms;
import :terms_enum;
import :types;
import :weight;

import std;

namespace lucene {

namespace {

/// `SpanTermQuery.termPositionsCost`: the cost of one position lookup, in the
/// same units the phrase query uses.
constexpr float kPhraseToSpanTermPositionsCost = 4.0F;
constexpr float kTermPositionsSeekOpsPerDoc = 128.0F;
constexpr float kTermOpsPerPos = 7.0F;

} // namespace

SpanTermWeight::SpanTermWeight(const SpanTermQuery &query, const IndexSearcher &searcher, TermStatesMap term_states,
                               float boost)
    : SpanWeight(query, searcher, term_states, boost), term_(query.term()) {
    const auto found = term_states.find(term_);
    if (found != term_states.end()) {
        term_states_ = found->second;
    }
}

VoidResult SpanTermWeight::extract_term_states(TermStatesMap &contexts) const {
    contexts[term_] = term_states_;
    return {};
}

Result<std::unique_ptr<Spans>> SpanTermWeight::get_spans(const LeafReaderContext &context, SpanPostings required) {
    if (term_states_ == nullptr) {
        return std::unique_ptr<Spans>{};
    }

    auto prepared = term_states_->get(context);
    if (!prepared.has_value()) {
        return std::unexpected(std::move(prepared.error()));
    }
    if (!prepared.value().has_value()) {
        // The term does not occur in this leaf.
        return std::unique_ptr<Spans>{};
    }
    auto resolved = term_states_->resolve(prepared.value().value());
    if (!resolved.has_value()) {
        return std::unexpected(std::move(resolved.error()));
    }
    if (!resolved.value().has_value()) {
        return std::unique_ptr<Spans>{};
    }
    const TermStateEnum *term_state = resolved.value().value();

    auto terms = context.reader()->terms(term_.field());
    if (!terms.has_value()) {
        return std::unexpected(std::move(terms.error()));
    }
    if (terms.value() == nullptr) {
        return std::unique_ptr<Spans>{};
    }
    if (!terms.value()->has_positions()) {
        return err(LuceneError::illegal_state("field \"" + term_.field() +
                                              "\" was indexed without position data; cannot run SpanTermQuery (term=" +
                                              term_.text() + ")"));
    }

    auto terms_enum = terms.value()->iterator();
    if (!terms_enum.has_value()) {
        return std::unexpected(std::move(terms_enum.error()));
    }
    if (VoidResult sought = terms_enum.value()->seek_exact_with_state(term_.bytes().as_slice(), *term_state);
        !sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
    }

    int16_t flags = POSITIONS;
    if (required == SpanPostings::kPayloads) {
        flags = PAYLOADS;
    } else if (required == SpanPostings::kOffsets) {
        flags = OFFSETS;
    }
    auto postings = terms_enum.value()->postings_with_flags(nullptr, flags);
    if (!postings.has_value()) {
        return std::unexpected(std::move(postings.error()));
    }

    auto doc_freq = terms_enum.value()->doc_freq();
    if (!doc_freq.has_value()) {
        return std::unexpected(std::move(doc_freq.error()));
    }
    auto total_term_freq = terms_enum.value()->total_term_freq();
    if (!total_term_freq.has_value()) {
        return std::unexpected(std::move(total_term_freq.error()));
    }
    if (doc_freq.value() <= 0 || total_term_freq.value() <= 0) {
        return std::unique_ptr<Spans>{};
    }
    const float expected_occurrences =
            static_cast<float>(total_term_freq.value()) / static_cast<float>(doc_freq.value());
    const float positions_cost =
            (kTermPositionsSeekOpsPerDoc + expected_occurrences * kTermOpsPerPos) * kPhraseToSpanTermPositionsCost;

    return std::unique_ptr<Spans>(std::make_unique<TermSpans>(std::move(postings.value()), term_, positions_cost));
}

SpanTermQuery::SpanTermQuery(Term term) : term_(std::move(term)) {}

Result<std::unique_ptr<Weight>> SpanTermQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                             float boost) const {
    const bool needs_scores = score_mode_needs_scores(score_mode);
    auto states = build_term_states(searcher, term_, needs_scores);
    if (!states.has_value()) {
        return std::unexpected(std::move(states.error()));
    }

    TermStatesMap term_states;
    term_states.emplace(term_, std::make_shared<TermStates>(std::move(states.value())));
    return std::unique_ptr<Weight>(
            std::make_unique<SpanTermWeight>(*this, searcher, std::move(term_states), boost));
}

VoidResult SpanTermQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(term_.field())) {
        const std::array<Term, 1> terms{term_};
        return visitor.consume_terms(*this, terms);
    }
    return {};
}

Result<std::string> SpanTermQuery::to_string(std::string_view field) const {
    if (field == term_.field()) {
        return "SpanTermQuery(" + term_.text() + ")";
    }
    return "SpanTermQuery(" + term_.to_string() + ")";
}

bool SpanTermQuery::equals(const Query &other) const {
    if (typeid(*this) != typeid(other)) {
        return false;
    }
    const auto &query = dynamic_cast<const SpanTermQuery &>(other);
    return term_ == query.term_;
}

size_t SpanTermQuery::hash() const {
    return std::hash<std::string>{}(term_.field()) * 31 + std::hash<std::string>{}(term_.text());
}

} // namespace lucene
