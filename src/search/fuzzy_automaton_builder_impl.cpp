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

module lucene_core:fuzzy_automaton_builder.impl;

import :fuzzy_automaton_builder;

import std;

import :automaton;
import :compiled_automaton;
import :error;
import :levenshtein_automata;
import :string_helper;
import :types;

namespace lucene {

FuzzyAutomatonBuilder::FuzzyAutomatonBuilder(std::string term, int32_t max_edits, LevenshteinAutomata lev_builder,
                                             std::string prefix, size_t term_length)
        : term_(std::move(term)), max_edits_(max_edits), lev_builder_(std::move(lev_builder)),
          prefix_(std::move(prefix)), term_length_(term_length) {}

Result<FuzzyAutomatonBuilder> FuzzyAutomatonBuilder::create(std::string term, int32_t max_edits,
                                                           size_t prefix_length, bool transpositions) {
    if (max_edits < 0 || max_edits > LevenshteinAutomata::MAXIMUM_SUPPORTED_DISTANCE) {
        return err(LuceneError::illegal_argument(
                std::format("max edits must be 0..{}, inclusive; got: {}", LevenshteinAutomata::MAXIMUM_SUPPORTED_DISTANCE,
                            max_edits)));
    }

    auto code_points = StringHelper::utf8_to_code_points(term);
    if (!code_points.has_value()) {
        return std::unexpected(std::move(code_points.error()));
    }
    const std::vector<int32_t> &points = code_points.value();
    const size_t term_length = points.size();
    const size_t prefix_size = std::min(prefix_length, term_length);

    // The prefix is not fuzzy; the rest of the word is.
    std::vector<int32_t> suffix(points.begin() + static_cast<std::ptrdiff_t>(prefix_size), points.end());
    auto lev_builder = LevenshteinAutomata::from_word(std::move(suffix), MAX_CODE_POINT, transpositions);
    if (!lev_builder.has_value()) {
        return std::unexpected(std::move(lev_builder.error()));
    }

    std::string prefix;
    for (size_t i = 0; i < prefix_size; ++i) {
        if (auto appended = StringHelper::utf8_append(prefix, points[i]); !appended.has_value()) {
            return std::unexpected(std::move(appended.error()));
        }
    }

    return FuzzyAutomatonBuilder(std::move(term), max_edits, std::move(lev_builder.value()), std::move(prefix),
                                 term_length);
}

Result<std::vector<CompiledAutomaton>> FuzzyAutomatonBuilder::build_automaton_set() const {
    std::vector<CompiledAutomaton> compiled;
    compiled.reserve(static_cast<size_t>(max_edits_) + 1);
    for (int32_t edits = 0; edits <= max_edits_; ++edits) {
        auto one = compile(static_cast<size_t>(edits));
        if (!one.has_value()) {
            return std::unexpected(std::move(one.error()));
        }
        compiled.push_back(std::move(one.value()));
    }
    return compiled;
}

Result<CompiledAutomaton> FuzzyAutomatonBuilder::build_max_edit_automaton() const {
    return compile(static_cast<size_t>(max_edits_));
}

Result<CompiledAutomaton> FuzzyAutomatonBuilder::compile(size_t edits) const {
    auto automaton = lev_builder_.to_automaton_with_prefix(edits, prefix_);
    if (!automaton.has_value()) {
        return std::unexpected(std::move(automaton.error()));
    }
    if (!automaton.value().has_value()) {
        return err(LuceneError::illegal_argument(std::format("unsupported edit distance: {}", edits)));
    }
    // The automaton is finite and already deterministic, so there is nothing to
    // simplify.
    auto compiled = CompiledAutomaton::create(std::move(automaton.value().value()), true, false);
    if (!compiled.has_value()) {
        if (compiled.error().kind() == ErrorKind::kTooComplexToDeterminize) {
            // Keep the cause, like the reference implementation's suppressed error.
            LuceneError fuzzy = LuceneError::fuzzy_terms(term_);
            fuzzy.add_suppressed(compiled.error());
            return std::unexpected(std::move(fuzzy));
        }
        return std::unexpected(std::move(compiled.error()));
    }
    return compiled;
}

} // namespace lucene
