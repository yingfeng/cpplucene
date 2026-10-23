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

module lucene_core:blended_term_query.impl;

import :blended_term_query;

import std;

import :boolean_clause;
import :boolean_query;
import :boost_query;
import :disjunction_max_query;
import :error;
import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :term;
import :term_query;
import :term_states;
import :types;
import :weight;

namespace lucene {

namespace {

/// Copies the per-leaf states of `states` into a fresh view and replaces the
/// accumulated statistics with the blended ones.
///
/// Port of `adjust_frequencies` (`core/search/blended_term_query.rs`).
Result<TermStates> adjust_frequencies(const IndexSearcher &searcher, TermStates &states, int32_t artificial_doc_freq,
                                      int64_t artificial_total_term_freq) {
    auto leaves = searcher.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    auto adjusted = TermStates::create(searcher.reader());
    if (!adjusted.has_value()) {
        return std::unexpected(std::move(adjusted.error()));
    }
    for (const LeafReaderContext &leaf : leaves.value()) {
        auto prepared = states.get(leaf);
        if (!prepared.has_value()) {
            return std::unexpected(std::move(prepared.error()));
        }
        if (prepared.value().has_value()) {
            auto resolved = states.resolve(prepared.value().value());
            if (!resolved.has_value()) {
                return std::unexpected(std::move(resolved.error()));
            }
            if (resolved.value().has_value()) {
                adjusted.value().register_state(*resolved.value().value(), leaf.ord());
            }
        }
    }
    adjusted.value().accumulate_statistics(artificial_doc_freq, artificial_total_term_freq);
    return adjusted;
}

} // namespace

// ---------------------------------------------------------------- Builder

VoidResult BlendedTermQuery::Builder::add(Term term, float boost) {
    return add_with_term_states(std::move(term), boost, nullptr);
}

VoidResult BlendedTermQuery::Builder::add_with_term_states(Term term, float boost, std::shared_ptr<TermStates> states) {
    if (terms_.size() >= BooleanQuery::max_clause_count()) {
        return err(LuceneError::too_many_clauses(
                std::format("maxClauseCount is set to {}", BooleanQuery::max_clause_count())));
    }
    terms_.push_back(std::move(term));
    boosts_.push_back(boost);
    states_.push_back(std::move(states));
    return {};
}

Result<BlendedTermQuery> BlendedTermQuery::Builder::build() {
    if (terms_.empty()) {
        return err(LuceneError::illegal_argument("a blended term query needs at least one term"));
    }
    return BlendedTermQuery(std::move(terms_), std::move(boosts_), std::move(states_), rewrite_method_,
                            tie_breaker_multiplier_);
}

// ---------------------------------------------------------------- BlendedTermQuery

BlendedTermQuery::BlendedTermQuery(std::vector<Term> terms, std::vector<float> boosts,
                                   std::vector<std::shared_ptr<TermStates>> states, RewriteMethod rewrite_method,
                                   float tie_breaker_multiplier)
        : terms_(std::move(terms)),
          boosts_(std::move(boosts)),
          states_(std::move(states)),
          rewrite_method_(rewrite_method),
          tie_breaker_multiplier_(tie_breaker_multiplier) {
    // The reference implementation sorts the terms when the query is built:
    // `InPlaceMergeSorter` over the three parallel arrays.
    std::vector<size_t> order(terms_.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::stable_sort(order, [this](size_t left, size_t right) {
        return terms_[left].compare_to(terms_[right]) < 0;
    });
    std::vector<Term> sorted_terms;
    std::vector<float> sorted_boosts;
    std::vector<std::shared_ptr<TermStates>> sorted_states;
    sorted_terms.reserve(order.size());
    sorted_boosts.reserve(order.size());
    sorted_states.reserve(order.size());
    for (const size_t index : order) {
        sorted_terms.push_back(std::move(terms_[index]));
        sorted_boosts.push_back(boosts_[index]);
        sorted_states.push_back(std::move(states_[index]));
    }
    terms_ = std::move(sorted_terms);
    boosts_ = std::move(sorted_boosts);
    states_ = std::move(sorted_states);
}

Result<std::unique_ptr<Weight>> BlendedTermQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const {
    (void)searcher;
    (void)score_mode;
    (void)boost;
    return err(LuceneError::unsupported_operation("a blended term query has to be rewritten first"));
}

Result<std::shared_ptr<Query>> BlendedTermQuery::rewrite(const IndexSearcher &searcher) const {
    // Fill in the missing term states and blend their statistics.
    std::vector<std::shared_ptr<TermStates>> contexts;
    contexts.reserve(states_.size());
    for (size_t i = 0; i < terms_.size(); ++i) {
        if (states_[i] != nullptr) {
            contexts.push_back(states_[i]);
            continue;
        }
        auto built = build_term_states(searcher, terms_[i], true);
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        contexts.push_back(std::make_shared<TermStates>(std::move(built.value())));
    }

    // The blended document frequency is the maximum of the term frequencies,
    // the blended total term frequency their sum.
    int32_t blended_doc_freq = 0;
    int64_t blended_total_term_freq = 0;
    for (const auto &states : contexts) {
        auto doc_freq = states->doc_freq();
        if (!doc_freq.has_value()) {
            return std::unexpected(std::move(doc_freq.error()));
        }
        auto total_term_freq = states->total_term_freq();
        if (!total_term_freq.has_value()) {
            return std::unexpected(std::move(total_term_freq.error()));
        }
        blended_doc_freq = std::max(blended_doc_freq, doc_freq.value());
        blended_total_term_freq += total_term_freq.value();
    }
    for (auto &states : contexts) {
        auto adjusted = adjust_frequencies(searcher, *states, blended_doc_freq, blended_total_term_freq);
        if (!adjusted.has_value()) {
            return std::unexpected(std::move(adjusted.error()));
        }
        states = std::make_shared<TermStates>(std::move(adjusted.value()));
    }

    std::vector<std::shared_ptr<Query>> term_queries;
    term_queries.reserve(terms_.size());
    for (size_t i = 0; i < terms_.size(); ++i) {
        std::shared_ptr<Query> term_query =
                std::make_shared<TermQuery>(terms_[i], std::make_shared<TermStates>(*contexts[i]));
        if (boosts_[i] != 1.0F) {
            auto boosted = BoostQuery::create(term_query, boosts_[i]);
            if (!boosted.has_value()) {
                return std::unexpected(std::move(boosted.error()));
            }
            term_query = std::make_shared<BoostQuery>(std::move(boosted.value()));
        }
        term_queries.push_back(std::move(term_query));
    }

    if (rewrite_method_ == RewriteMethod::kDisjunctionMax) {
        auto disjunction = DisjunctionMaxQuery::create(std::move(term_queries), tie_breaker_multiplier_);
        if (!disjunction.has_value()) {
            return std::unexpected(std::move(disjunction.error()));
        }
        return std::static_pointer_cast<Query>(disjunction.value());
    }

    BooleanQuery::Builder builder;
    for (auto &term_query : term_queries) {
        if (auto added = builder.add(std::move(term_query), Occur::Should); !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
    }
    auto built = builder.build();
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    return std::shared_ptr<Query>(new BooleanQuery(std::move(built.value())));
}

VoidResult BlendedTermQuery::visit(QueryVisitor &visitor) const {
    std::vector<Term> accepted;
    for (const Term &term : terms_) {
        if (visitor.accept_field(term.field())) {
            accepted.push_back(term);
        }
    }
    if (accepted.empty()) {
        return {};
    }
    // **Deviation:** the reference implementation visits through a sub visitor for
    // the `SHOULD` occurrence; the port has no `get_sub_visitor` yet.
    return visitor.consume_terms(*this, accepted);
}

Result<std::string> BlendedTermQuery::to_string(std::string_view field) const {
    std::string builder = "Blended(";
    for (size_t i = 0; i < terms_.size(); ++i) {
        if (i != 0) {
            builder.push_back(' ');
        }
        std::shared_ptr<Query> term_query = std::make_shared<TermQuery>(terms_[i]);
        if (boosts_[i] != 1.0F) {
            auto boosted = BoostQuery::create(term_query, boosts_[i]);
            if (!boosted.has_value()) {
                return std::unexpected(std::move(boosted.error()));
            }
            term_query = std::make_shared<BoostQuery>(std::move(boosted.value()));
        }
        auto text = term_query->to_string(field);
        if (!text.has_value()) {
            return std::unexpected(std::move(text.error()));
        }
        builder += text.value();
    }
    builder.push_back(')');
    return builder;
}

bool BlendedTermQuery::equals(const Query &other) const {
    const auto *blended = dynamic_cast<const BlendedTermQuery *>(&other);
    if (blended == nullptr) {
        return false;
    }
    if (terms_.size() != blended->terms_.size() || boosts_.size() != blended->boosts_.size()) {
        return false;
    }
    if (rewrite_method_ != blended->rewrite_method_ ||
        std::bit_cast<uint32_t>(tie_breaker_multiplier_) !=
                std::bit_cast<uint32_t>(blended->tie_breaker_multiplier_)) {
        return false;
    }
    for (size_t i = 0; i < terms_.size(); ++i) {
        if (!(terms_[i] == blended->terms_[i])) {
            return false;
        }
        if (std::bit_cast<uint32_t>(boosts_[i]) != std::bit_cast<uint32_t>(blended->boosts_[i])) {
            return false;
        }
        const bool left_null = states_[i] == nullptr;
        const bool right_null = blended->states_[i] == nullptr;
        if (left_null != right_null) {
            return false;
        }
        if (!left_null && !(*states_[i] == *blended->states_[i])) {
            return false;
        }
    }
    return true;
}

size_t BlendedTermQuery::hash() const {
    size_t result = 0x9E3779B9;
    for (size_t i = 0; i < terms_.size(); ++i) {
        result = result * 31 + std::hash<std::string>{}(terms_[i].field());
        result = result * 31 + std::hash<std::string>{}(terms_[i].bytes().to_display_string());
        result = result * 31 + static_cast<size_t>(std::bit_cast<uint32_t>(boosts_[i]));
    }
    result = result * 31 + static_cast<size_t>(rewrite_method_);
    result = result * 31 + static_cast<size_t>(std::bit_cast<uint32_t>(tie_breaker_multiplier_));
    return result;
}

} // namespace lucene
