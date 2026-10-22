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

module lucene_core:levenshtein_automata.impl;

import :levenshtein_automata;

import :automata;
import :automaton;
import :debug;
import :error;
import :lev_parametric_description;
import :operations;
import :parametric_description;
import :string_helper;
import :types;

import std;

namespace lucene {

Result<std::vector<int32_t>> LevenshteinAutomata::code_points(std::string_view input) {
    return StringHelper::utf8_to_code_points(input);
}

Result<LevenshteinAutomata> LevenshteinAutomata::create(std::string_view input, bool with_transpositions) {
    auto word = code_points(input);
    if (!word) {
        return err(std::move(word.error()));
    }
    return from_word(std::move(word.value()), MAX_CODE_POINT, with_transpositions);
}

Result<LevenshteinAutomata> LevenshteinAutomata::from_word(std::vector<int32_t> word, int32_t alpha_max,
                                                           bool with_transpositions) {
    LevenshteinAutomata result;
    result.alpha_max_ = alpha_max;

    result.alphabet_.reserve(word.size());
    for (const int32_t value : word) {
        if (value > alpha_max) {
            return err(LuceneError::illegal_argument(
                    std::format("alphaMax exceeded by symbol {} in word", value)));
        }
        result.alphabet_.push_back(value);
    }
    std::sort(result.alphabet_.begin(), result.alphabet_.end());
    result.alphabet_.erase(std::unique(result.alphabet_.begin(), result.alphabet_.end()), result.alphabet_.end());

    result.range_lower_.assign(result.alphabet_.size() + 2, 0);
    result.range_upper_.assign(result.alphabet_.size() + 2, 0);

    int32_t lower = 0;
    for (const int32_t higher : result.alphabet_) {
        if (higher > lower) {
            result.range_lower_[result.num_ranges_] = lower;
            result.range_upper_[result.num_ranges_] = higher - 1;
            result.num_ranges_ += 1;
        }
        lower = higher + 1;
    }
    if (lower <= alpha_max) {
        result.range_lower_[result.num_ranges_] = lower;
        result.range_upper_[result.num_ranges_] = alpha_max;
        result.num_ranges_ += 1;
    }

    const auto w = static_cast<int32_t>(word.size());
    result.word_ = std::move(word);
    result.descriptions_[0] = std::nullopt;
    result.descriptions_[1] = with_transpositions ? create_lev1t_description(w) : create_lev1_description(w);
    result.descriptions_[2] = with_transpositions ? create_lev2t_description(w) : create_lev2_description(w);

    return result;
}

int32_t LevenshteinAutomata::get_vector(int32_t x, int32_t pos, int32_t end) const {
    int32_t vector = 0;
    for (int32_t i = pos; i < end; ++i) {
        vector <<= 1;
        if (word_[static_cast<size_t>(i)] == x) {
            vector |= 1;
        }
    }
    return vector;
}

Result<std::optional<Automaton>> LevenshteinAutomata::to_automaton(size_t n) const {
    return to_automaton_with_prefix(n, std::string_view());
}

Result<std::optional<Automaton>> LevenshteinAutomata::to_automaton_with_prefix(size_t n, std::string_view prefix) const {
    if (n == 0) {
        std::string exact(prefix);
        for (const int32_t code_point : word_) {
            if (auto appended = StringHelper::utf8_append(exact, code_point); !appended) {
                return err(LuceneError::illegal_argument(
                        std::format("invalid Unicode code point {} in word", code_point)));
            }
        }
        auto automaton = Automata::make_string(exact);
        if (!automaton) {
            return err(std::move(automaton.error()));
        }
        return std::optional<Automaton>(std::move(automaton.value()));
    }

    if (n >= descriptions_.size() || !descriptions_[n].has_value()) {
        return std::optional<Automaton>{};
    }

    const ParametricDescription &description = descriptions_[n].value();
    const auto range = static_cast<int32_t>(2 * n + 1);
    const int32_t num_states = description.size();
    const auto num_transitions =
            static_cast<size_t>(num_states) *
            static_cast<size_t>(std::min(static_cast<int32_t>(1 + 2 * n), static_cast<int32_t>(alphabet_.size())));

    auto prefix_code_points = StringHelper::utf8_to_code_points(prefix);
    if (!prefix_code_points) {
        return err(std::move(prefix_code_points.error()));
    }
    const auto prefix_states = static_cast<int32_t>(prefix_code_points->size());

    Automaton a = Automaton::with_capacity(static_cast<size_t>(num_states + prefix_states), num_transitions);
    int32_t last_state = a.create_state();
    for (const int32_t code_point : prefix_code_points.value()) {
        const int32_t state = a.create_state();
        if (auto added = a.add_transition_label(last_state, state, code_point); !added) {
            return err(std::move(added.error()));
        }
        last_state = state;
    }

    const int32_t state_offset = last_state;
    a.set_accept(last_state, description.is_accept(0));

    for (int32_t i = 1; i < num_states; ++i) {
        const int32_t state = a.create_state();
        a.set_accept(state, description.is_accept(i));
    }

    for (int32_t k = 0; k < num_states; ++k) {
        const int32_t xpos = description.get_position(k);
        if (xpos < 0) {
            continue;
        }
        const auto word_length = static_cast<int32_t>(word_.size());
        const int32_t end = xpos + std::min(word_length - xpos, range);

        for (const int32_t codepoint : alphabet_) {
            const int32_t cvec = get_vector(codepoint, xpos, end);
            const int32_t dest = description.transition(k, xpos, cvec);
            if (dest >= 0) {
                if (auto added = a.add_transition_label(state_offset + k, state_offset + dest, codepoint); !added) {
                    return err(std::move(added.error()));
                }
            }
        }

        // By definition the characteristic vector is 0 for every codepoint that
        // does not occur in the input word.
        const int32_t dest = description.transition(k, xpos, 0);
        if (dest >= 0) {
            for (size_t r = 0; r < num_ranges_; ++r) {
                if (auto added = a.add_transition(state_offset + k, state_offset + dest, range_lower_[r],
                                                  range_upper_[r]);
                    !added) {
                    return err(std::move(added.error()));
                }
            }
        }
    }

    if (auto finished = a.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }

    auto reduced = Operations::remove_dead_states(a);
    if (!reduced) {
        return err(std::move(reduced.error()));
    }
    Automaton automaton = reduced->has_value() ? std::move(reduced->value()) : std::move(a);
    debug_check(automaton.is_deterministic(), "a Levenshtein automaton is deterministic");
    return std::optional<Automaton>(std::move(automaton));
}

} // namespace lucene
