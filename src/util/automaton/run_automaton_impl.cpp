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

module lucene_core:run_automaton.impl;

import :run_automaton;

import :automaton;
import :debug;
import :error;
import :transition;
import :types;

import std;

namespace lucene {

Result<RunAutomaton> RunAutomaton::create(Automaton automaton, size_t alphabet_size) {
    if (!automaton.is_deterministic()) {
        return err(LuceneError::illegal_argument("Automaton must be deterministic"));
    }

    RunAutomaton run_automaton;
    run_automaton.points_ = automaton.get_start_points();
    const int32_t automaton_states = automaton.get_num_states();
    const int32_t state_count = std::max(1, automaton_states);
    run_automaton.size_ = static_cast<size_t>(state_count);
    run_automaton.alphabet_size_ = alphabet_size;
    run_automaton.accept_ = FixedBitSet(static_cast<size_t>(state_count));
    run_automaton.transitions_.assign(static_cast<size_t>(state_count) * run_automaton.points_.size(), -1);

    Transition transition;
    // The empty language keeps one rejecting run state but has no source state
    // to read from the automaton.
    for (int32_t state = 0; state < automaton_states; ++state) {
        const auto state_index = static_cast<size_t>(state);
        if (automaton.is_accept(state)) {
            run_automaton.accept_.set(state_index);
        }
        transition.source = state;
        transition.transition_upto.reset();

        for (size_t c_index = 0; c_index < run_automaton.points_.size(); ++c_index) {
            const int32_t dest = automaton.next(transition, run_automaton.points_[c_index]);
            debug_check(dest == -1 || dest < state_count, "the destination fits in the run states");
            run_automaton.transitions_[state_index * run_automaton.points_.size() + c_index] = dest;
        }
    }

    run_automaton.classmap_.assign(std::min<size_t>(alphabet_size, 256), 0);
    size_t class_index = 0;
    for (size_t codepoint = 0; codepoint < run_automaton.classmap_.size(); ++codepoint) {
        if (class_index + 1 < run_automaton.points_.size() &&
            static_cast<int32_t>(codepoint) == run_automaton.points_[class_index + 1]) {
            class_index += 1;
        }
        run_automaton.classmap_[codepoint] = class_index;
    }

    run_automaton.automaton_ = std::move(automaton);
    return run_automaton;
}

Result<bool> RunAutomaton::is_accept(int32_t state) const {
    if (state < 0) {
        return err(LuceneError::array_index_out_of_bounds(std::format("state: {}", state)));
    }
    return accept_.get(static_cast<size_t>(state));
}

size_t RunAutomaton::get_char_class(int32_t c) const noexcept {
    size_t a = 0;
    size_t b = points_.size();
    while (b - a > 1) {
        const size_t d = (a + b) / 2;
        if (points_[d] > c) {
            b = d;
        } else if (points_[d] < c) {
            a = d;
        } else {
            return d;
        }
    }
    return a;
}

Result<int32_t> RunAutomaton::step(int32_t state, int32_t c) const {
    debug_check(c >= 0 && static_cast<size_t>(c) < alphabet_size_, "the codepoint is inside the alphabet");
    const auto char_index = static_cast<size_t>(c);
    const size_t char_class = char_index >= classmap_.size() ? get_char_class(c) : classmap_[char_index];
    const size_t index = static_cast<size_t>(state) * points_.size() + char_class;
    if (index >= transitions_.size()) {
        return err(LuceneError::array_index_out_of_bounds(
                std::format("state: {}, class: {}, states: {}", state, char_class, size_)));
    }
    return transitions_[index];
}

std::string RunAutomaton::to_dot() const {
    std::string out = "initial state: 0\n";
    for (size_t state = 0; state < size_; ++state) {
        out += std::format("state {}", state);
        out += accept_.get_bit(state) ? " [accept]:" : " [reject]:";
        for (size_t i = 0; i < points_.size(); ++i) {
            const int32_t dest = transitions_[state * points_.size() + i];
            if (dest == -1) {
                continue;
            }
            const int32_t min = points_[i];
            const auto max = i + 1 < points_.size() ? points_[i + 1] - 1 : static_cast<int32_t>(alphabet_size_);
            if (min == max) {
                out += std::format(" {}->{}", min, dest);
            } else {
                out += std::format(" {}-{}->{}", min, max, dest);
            }
        }
        if (state + 1 < size_) {
            out += '\n';
        }
    }
    return out;
}

} // namespace lucene
