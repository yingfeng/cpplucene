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

module lucene_core:fuzzy_query.impl;

import :fuzzy_query;

import std;

import :compiled_automaton;
import :error;
import :fuzzy_automaton_builder;
import :levenshtein_automata;
import :multi_term_query;
import :query;
import :single_terms_enum;
import :term;
import :terms;
import :terms_enum;
import :types;

namespace lucene {

FuzzyQuery::FuzzyQuery(Term term, int32_t max_edits, size_t prefix_length, size_t max_expansions, bool transpositions,
                       MultiTermRewriteMethod rewrite_method)
        : MultiTermQuery(term.field(), term.text(), rewrite_method), term_(std::move(term)), max_edits_(max_edits),
          prefix_length_(prefix_length), max_expansions_(max_expansions), transpositions_(transpositions) {}

Result<FuzzyQuery> FuzzyQuery::create(Term term) {
    return with_max_edits(std::move(term), DEFAULT_MAX_EDITS);
}

Result<FuzzyQuery> FuzzyQuery::with_max_edits(Term term, int32_t max_edits) {
    return with_max_edits_and_prefix(std::move(term), max_edits, DEFAULT_PREFIX_LENGTH);
}

Result<FuzzyQuery> FuzzyQuery::with_max_edits_and_prefix(Term term, int32_t max_edits, size_t prefix_length) {
    return with_options(std::move(term), max_edits, prefix_length, DEFAULT_MAX_EXPANSIONS, DEFAULT_TRANSPOSITIONS);
}

Result<FuzzyQuery> FuzzyQuery::with_options(Term term, int32_t max_edits, size_t prefix_length, size_t max_expansions,
                                            bool transpositions) {
    return with_rewrite(std::move(term), max_edits, prefix_length, max_expansions, transpositions,
                        MultiTermRewriteMethod::kConstantScore);
}

Result<FuzzyQuery> FuzzyQuery::with_rewrite(Term term, int32_t max_edits, size_t prefix_length, size_t max_expansions,
                                            bool transpositions, MultiTermRewriteMethod rewrite_method) {
    if (max_edits < 0 || max_edits > LevenshteinAutomata::MAXIMUM_SUPPORTED_DISTANCE) {
        return err(LuceneError::illegal_argument(
                std::format("maxEdits must be between 0 and {}", LevenshteinAutomata::MAXIMUM_SUPPORTED_DISTANCE)));
    }
    if (max_expansions == 0) {
        return err(LuceneError::illegal_argument("maxExpansions must be positive."));
    }
    return FuzzyQuery(std::move(term), max_edits, prefix_length, max_expansions, transpositions, rewrite_method);
}

Result<CompiledAutomaton> FuzzyQuery::get_fuzzy_automaton(std::string term, int32_t max_edits, size_t prefix_length,
                                                          bool transpositions) {
    auto builder = FuzzyAutomatonBuilder::create(std::move(term), max_edits, prefix_length, transpositions);
    if (!builder.has_value()) {
        return std::unexpected(std::move(builder.error()));
    }
    return builder.value().build_max_edit_automaton();
}

Result<CompiledAutomaton> FuzzyQuery::automaton() const {
    return get_fuzzy_automaton(term_.text(), max_edits_, prefix_length_, transpositions_);
}

int32_t FuzzyQuery::float_to_edits(float minimum_similarity, size_t term_len) {
    if (minimum_similarity >= 1.0F) {
        return static_cast<int32_t>(
                std::min(minimum_similarity, static_cast<float>(LevenshteinAutomata::MAXIMUM_SUPPORTED_DISTANCE)));
    }
    if (minimum_similarity == 0.0F) {
        return 0;
    }
    const auto scaled = static_cast<int32_t>((1.0 - static_cast<double>(minimum_similarity)) *
                                             static_cast<double>(term_len));
    return std::min(scaled, LevenshteinAutomata::MAXIMUM_SUPPORTED_DISTANCE);
}

Result<std::unique_ptr<TermsEnum>> FuzzyQuery::terms_enum(Terms &terms) const {
    if (max_edits_ == 0) {
        // No edit is allowed, so the query is the single term itself.
        auto inner = terms.iterator();
        if (!inner.has_value()) {
            return std::unexpected(std::move(inner.error()));
        }
        return std::unique_ptr<TermsEnum>(SingleTermsEnum::create(std::move(inner.value()), term_.bytes()));
    }
    auto compiled = automaton();
    if (!compiled.has_value()) {
        return std::unexpected(std::move(compiled.error()));
    }
    return terms.terms_enum_for(compiled.value());
}

bool FuzzyQuery::peers(const MultiTermQuery &other) const {
    const auto *fuzzy = dynamic_cast<const FuzzyQuery *>(&other);
    if (fuzzy == nullptr) {
        return false;
    }
    return term_ == fuzzy->term_ && max_edits_ == fuzzy->max_edits_ && prefix_length_ == fuzzy->prefix_length_ &&
           max_expansions_ == fuzzy->max_expansions_ && transpositions_ == fuzzy->transpositions_ &&
           MultiTermQuery::peers(other);
}

Result<std::string> FuzzyQuery::to_string(std::string_view field) const {
    const std::string body = std::format("{}~{}", term_.text(), max_edits_);
    if (!field.empty() && field == term_.field()) {
        return body;
    }
    return std::format("{}:{}", term_.field(), body);
}

} // namespace lucene
