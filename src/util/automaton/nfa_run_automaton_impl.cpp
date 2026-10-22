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

module lucene_core:nfa_run_automaton.impl;

import :nfa_run_automaton;

import :array_util;
import :automaton;
import :bit_mixer;
import :byte_runnable;
import :debug;
import :error;
import :point_transition_set;
import :state_set;
import :transition;
import :types;

import std;

namespace lucene {

Result<NFARunAutomaton> NFARunAutomaton::create(Automaton automaton) {
    return create_with_alphabet_size(std::move(automaton), 0x10FFFF + 1);
}

Result<NFARunAutomaton> NFARunAutomaton::create_with_alphabet_size(Automaton automaton, int32_t alphabet_size) {
    NFARunAutomaton result;
    if (auto initialized = result.init(std::move(automaton), alphabet_size); !initialized) {
        return err(std::move(initialized.error()));
    }
    return result;
}

VoidResult NFARunAutomaton::init(Automaton automaton, int32_t alphabet_size) {
    automaton_ = std::move(automaton);
    points_ = automaton_.get_start_points();

    const auto classmap_len = static_cast<size_t>(std::min<int32_t>(256, alphabet_size));
    classmap_.assign(classmap_len, 0);
    size_t i = 0;
    for (size_t j = 0; j < classmap_len; ++j) {
        if (i + 1 < points_.size() && static_cast<int32_t>(j) == points_[i + 1]) {
            i += 1;
        }
        classmap_[j] = i;
    }

    alphabet_size_ = alphabet_size;
    dstates_.reserve(10);
    dstate_slots_ = 0;

    auto initial_state = make_dstate(std::vector<int32_t>{0});
    if (auto ord = find_dstate(std::move(initial_state)); !ord) {
        return err(std::move(ord.error()));
    }

    return {};
}

NFARunAutomaton::DState NFARunAutomaton::make_dstate(std::vector<int32_t> nfa_states) const {
    debug_check(!nfa_states.empty(), "a DFA state stands for at least one NFA state");

    uint32_t hash_code = static_cast<uint32_t>(nfa_states.size());
    bool is_accept = false;
    for (const int32_t state : nfa_states) {
        hash_code = hash_code + BitMixer::mix_i32(state);
        if (automaton_.is_accept(state)) {
            is_accept = true;
        }
    }

    DState dstate;
    dstate.nfa_states = std::move(nfa_states);
    dstate.transitions.assign(points_.size(), NOT_COMPUTED);
    dstate.hash_code = hash_code;
    dstate.is_accept = is_accept;
    return dstate;
}

Result<int32_t> NFARunAutomaton::find_dstate(std::optional<DState> dstate) {
    if (!dstate.has_value()) {
        return MISSING;
    }

    DStateKey key{dstate->nfa_states, static_cast<int32_t>(dstate->hash_code)};
    const size_t ord = state_.dstate_to_ord.size();
    const auto found = state_.dstate_to_ord.find(key);
    if (found != state_.dstate_to_ord.end()) {
        return found->second;
    }
    state_.dstate_to_ord.emplace(std::move(key), static_cast<int32_t>(ord));

    if (ord >= dstate_slots_) {
        // ByteRunnable::get_size exposes the historical slot growth, separately
        // from the number of initialized states.
        size_t slots = dstates_.capacity();
        if (slots <= ord) {
            auto oversized = ArrayUtil::oversize(ord + 1, sizeof(DState));
            if (!oversized) {
                return err(std::move(oversized.error()));
            }
            slots = oversized.value();
        }
        dstates_.reserve(slots);
        dstate_slots_ = slots;
    }

    debug_check(ord == dstates_.size(), "a new DFA state is appended");
    dstates_.push_back(std::move(dstate.value()));

    return static_cast<int32_t>(ord);
}

Result<size_t> NFARunAutomaton::get_char_class(int32_t c) const {
    if (c < 0 || c >= alphabet_size_) {
        return err(LuceneError::illegal_argument(
                std::format("codepoint {} is outside the alphabet of size {}", c, alphabet_size_)));
    }

    if (static_cast<size_t>(c) < classmap_.size()) {
        return classmap_[static_cast<size_t>(c)];
    }

    // Binary search over the interval start points.
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

VoidResult NFARunAutomaton::set_transition_accordingly(Transition &t) const {
    if (!t.transition_upto.has_value()) {
        return err(LuceneError::illegal_state("transition cursor is not initialized"));
    }
    const size_t transition_upto = t.transition_upto.value();
    const DState &state = dstates_[static_cast<size_t>(t.source)];
    t.dest = state.transitions[transition_upto];
    t.min = points_[transition_upto];

    if (transition_upto == points_.size() - 1) {
        t.max = alphabet_size_ - 1;
    } else {
        t.max = points_[transition_upto + 1] - 1;
    }
    return {};
}

Result<int32_t> NFARunAutomaton::next_state(size_t char_class, size_t index) {
    const size_t len = points_.size();
    dstates_[index].init_transitions(len);
    const int32_t cached = dstates_[index].transitions[char_class];

    if (cached == NOT_COMPUTED) {
        auto next_dstate = step_with_index(points_[char_class], index);
        if (!next_dstate) {
            return err(std::move(next_dstate.error()));
        }
        auto ord = find_dstate(std::move(next_dstate.value()));
        if (!ord) {
            return err(std::move(ord.error()));
        }
        dstates_[index].assign_transition(char_class, ord.value());

        // The step may have covered more than one char class.
        auto minimal_transition = std::exchange(dstates_[index].minimal_transition, std::nullopt);
        if (minimal_transition.has_value()) {
            const int32_t assigned = dstates_[index].transitions[char_class];
            size_t cls = char_class;
            while (cls > 0 && points_[cls - 1] >= minimal_transition->min) {
                cls -= 1;
                debug_check(dstates_[index].transitions[cls] == NOT_COMPUTED ||
                                    dstates_[index].transitions[cls] == assigned,
                            "the propagated transition agrees");
                dstates_[index].assign_transition(cls, assigned);
            }

            cls = char_class;
            while (cls + 1 < points_.size() && points_[cls + 1] <= minimal_transition->max) {
                cls += 1;
                debug_check(dstates_[index].transitions[cls] == NOT_COMPUTED ||
                                    dstates_[index].transitions[cls] == assigned,
                            "the propagated transition agrees");
                dstates_[index].assign_transition(cls, assigned);
            }
        }
    }

    return dstates_[index].transitions[char_class];
}

Result<std::optional<NFARunAutomaton::DState>> NFARunAutomaton::step_with_index(int32_t c, size_t index) {
    states_set_.reset();

    const std::vector<int32_t> &nfa_states = dstates_[index].nfa_states;
    int32_t left = -1;
    int32_t right = alphabet_size_;
    Transition step_transition;

    for (const int32_t nfa_state : nfa_states) {
        const int32_t num_transitions = automaton_.init_transition(nfa_state, step_transition);
        for (int32_t i = 0; i < num_transitions; ++i) {
            if (auto next = automaton_.get_next_transition(step_transition); !next) {
                return err(std::move(next.error()));
            }

            if (step_transition.min <= c && c <= step_transition.max) {
                states_set_.incr(step_transition.dest);
                left = std::max(left, step_transition.min);
                right = std::min(right, step_transition.max);
            }

            if (step_transition.max < c) {
                left = std::max(left, step_transition.max + 1);
            }

            if (step_transition.min > c) {
                right = std::min(right, step_transition.min - 1);
                break; // transitions are sorted
            }
        }
    }

    if (states_set_.size() == 0) {
        return std::optional<DState>{};
    }

    Transition minimal;
    minimal.min = left;
    minimal.max = right;
    dstates_[index].minimal_transition = minimal;

    return std::optional<DState>(make_dstate(states_set_.get_array()));
}

Result<int32_t> NFARunAutomaton::step_with_dstate_index(size_t dstate_index, int32_t c) {
    auto char_class = get_char_class(c);
    if (!char_class) {
        return err(std::move(char_class.error()));
    }
    return next_state(char_class.value(), dstate_index);
}

VoidResult NFARunAutomaton::determinize(size_t index) {
    const size_t len = points_.size();
    if (dstates_[index].computed_transitions == dstates_[index].transitions.size()) {
        return {};
    }
    dstates_[index].init_transitions(len);
    state_.transition_set.reset();

    {
        const std::vector<int32_t> &nfa_states = dstates_[index].nfa_states;
        Transition step_transition;
        for (const int32_t nfa_state : nfa_states) {
            const int32_t num_transitions = automaton_.init_transition(nfa_state, step_transition);
            for (int32_t i = 0; i < num_transitions; ++i) {
                if (auto next = automaton_.get_next_transition(step_transition); !next) {
                    return err(std::move(next.error()));
                }
                state_.transition_set.add(step_transition);
            }
        }
    }

    if (state_.transition_set.count == 0) {
        std::ranges::fill(dstates_[index].transitions, MISSING);
        dstates_[index].computed_transitions = dstates_[index].transitions.size();
        return {};
    }

    state_.transition_set.sort();
    states_set_.reset();

    int32_t last_point = -1;
    size_t char_class = 0;

    const size_t count = state_.transition_set.count;
    for (size_t i = 0; i < count; ++i) {
        const int32_t point = state_.transition_set.points[i].point;

        if (states_set_.size() > 0) {
            debug_check(last_point != -1, "a point follows another one");

            // `find_dstate` may grow `dstates`, so no reference to it is kept
            // across the call.
            auto ord = find_dstate(make_dstate(states_set_.get_array()));
            if (!ord) {
                return err(std::move(ord.error()));
            }
            const int32_t dest = ord.value();

            while (char_class < points_.size() && points_[char_class] < last_point) {
                dstates_[index].assign_transition(char_class, MISSING);
                char_class += 1;
            }

            debug_check(char_class < points_.size() && points_[char_class] == last_point,
                        "the char class starts at the last point");

            while (char_class < points_.size() && points_[char_class] < point) {
                debug_check(dstates_[index].transitions[char_class] == NOT_COMPUTED ||
                                    dstates_[index].transitions[char_class] == dest,
                            "the assigned transition agrees");
                dstates_[index].assign_transition(char_class, dest);
                char_class += 1;
            }

            debug_check((char_class == points_.size() && point == alphabet_size_) ||
                                points_[char_class] == point,
                        "the char class ends at this point");
        }

        // Transitions that end on this point close an overlapping interval.
        {
            TransitionList &ends = state_.transition_set.points[i].ends;
            const size_t limit = ends.next;
            for (size_t j = 0; j < limit; j += 3) {
                if (auto decr = states_set_.decr(ends.transitions[j]); !decr) {
                    return decr;
                }
            }
            ends.next = 0;
        }

        // Transitions that start on this point open a new interval.
        {
            TransitionList &starts = state_.transition_set.points[i].starts;
            const size_t limit = starts.next;
            for (size_t j = 0; j < limit; j += 3) {
                states_set_.incr(starts.transitions[j]);
            }
            last_point = point;
            starts.next = 0;
        }
    }

    debug_check(states_set_.size() == 0, "every state left the set");
    debug_check(dstates_[index].computed_transitions >= char_class,
                "at most one transition per char class was computed");

    // Transitions after `char_class` may already have been explored; the rest of
    // the table has no outgoing transition.  (the reference implementation fills `NOT_COMPUTED` here,
    // which contradicts its own comment and makes `get_transition` report a
    // transition whose destination is `NOT_COMPUTED`; `MISSING` is what the
    // iteration contract needs.)
    std::ranges::fill(dstates_[index].transitions.begin() + static_cast<std::ptrdiff_t>(char_class),
                      dstates_[index].transitions.end(), MISSING);
    dstates_[index].computed_transitions = dstates_[index].transitions.size();
    return {};
}

Result<int32_t> NFARunAutomaton::init_transition(int32_t state, Transition &t) {
    t.source = state;
    t.transition_upto = std::nullopt;
    return get_num_transitions_with_state(state);
}

VoidResult NFARunAutomaton::get_next_transition(Transition &t) {
    const std::vector<int32_t> &transitions = dstates_[static_cast<size_t>(t.source)].transitions;

    while (true) {
        const size_t idx = t.transition_upto.has_value() ? t.transition_upto.value() + 1 : 0;
        if (idx >= transitions.size()) {
            return err(LuceneError::illegal_state("no more transitions"));
        }
        t.transition_upto = idx;
        if (transitions[idx] != MISSING) {
            break;
        }
    }

    return set_transition_accordingly(t);
}

Result<int32_t> NFARunAutomaton::get_num_transitions_with_state(int32_t state) {
    const auto state_index = static_cast<size_t>(state);
    if (state < 0 || state_index >= dstates_.size()) {
        return err(LuceneError::illegal_argument(std::format("state {} does not exist", state)));
    }
    if (auto determined = determinize(state_index); !determined) {
        return err(std::move(determined.error()));
    }
    return dstates_[state_index].outgoing_transitions;
}

VoidResult NFARunAutomaton::get_transition(int32_t state, int32_t index, Transition &t) {
    const auto state_index = static_cast<size_t>(state);
    if (state < 0 || state_index >= dstates_.size()) {
        return err(LuceneError::illegal_argument(std::format("state {} does not exist", state)));
    }
    if (auto determined = determinize(state_index); !determined) {
        return determined;
    }

    t.transition_upto = std::nullopt;
    t.source = state;

    int32_t outgoing_transitions = -1;
    while (outgoing_transitions < index &&
           (t.transition_upto.has_value() ? t.transition_upto.value() + 1 : 0) < points_.size()) {
        const size_t idx = t.transition_upto.has_value() ? t.transition_upto.value() + 1 : 0;
        t.transition_upto = idx;
        if (dstates_[state_index].transitions[idx] != MISSING) {
            outgoing_transitions += 1;
        }
    }

    if (outgoing_transitions != index) {
        return err(LuceneError::illegal_argument(std::format("state {} has no transition {}", state, index)));
    }

    return set_transition_accordingly(t);
}

Result<int32_t> NFARunAutomaton::step(int32_t state, int32_t c) {
    const auto state_index = static_cast<size_t>(state);
    if (state < 0 || state_index >= dstates_.size()) {
        return err(LuceneError::illegal_argument(std::format("state {} does not exist", state)));
    }
    return step_with_dstate_index(state_index, c);
}

Result<bool> NFARunAutomaton::is_accept(int32_t state) const {
    const auto state_index = static_cast<size_t>(state);
    if (state < 0 || state_index >= dstates_.size()) {
        return err(LuceneError::illegal_argument(std::format("state {} does not exist", state)));
    }
    return dstates_[state_index].is_accept;
}

Result<bool> NFARunAutomaton::run_codepoints(std::span<const int32_t> input) {
    int32_t p = 0;
    for (const int32_t c : input) {
        auto next = step(p, c);
        if (!next) {
            return err(std::move(next.error()));
        }
        p = next.value();
        if (p == MISSING) {
            return false;
        }
    }
    return is_accept(p);
}

} // namespace lucene
