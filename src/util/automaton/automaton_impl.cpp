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

module lucene_core:automaton.impl;

import :automaton;

import :debug;
import :error;
import :transition;
import :types;

import std;

namespace lucene {

namespace {

/// One transition as the automaton stores it: `{dest, min, max}`.
using PackedTransition = std::array<int32_t, 3>;

} // namespace

Automaton::Automaton() {
    states_.assign(4, 0);
    transitions_.reserve(6);
}

Automaton Automaton::with_capacity(size_t num_states, size_t num_transitions) {
    Automaton automaton;
    automaton.states_.assign(num_states * 2, 0);
    automaton.transitions_.reserve(num_transitions * 3);
    return automaton;
}

void Automaton::grow_states() {
    const size_t len = next_state_ + 2;
    if (len > states_.size()) {
        states_.resize(len, 0);
    }
}

void Automaton::grow_transitions() {
    const size_t len = next_transition_ + 3;
    if (len > transitions_.size()) {
        transitions_.resize(len, 0);
    }
}

int32_t Automaton::create_state() {
    grow_states();
    const auto state = static_cast<int32_t>(next_state_ / 2);
    states_[next_state_] = -1;
    next_state_ += 2;
    return state;
}

void Automaton::set_accept(int32_t state, bool accept) {
    debug_check(state >= 0 && state < get_num_states(), "the state must exist");
    const auto index = static_cast<size_t>(state);
    if (is_accept_.size() <= index) {
        is_accept_.resize(index + 1, false);
    }
    is_accept_[index] = accept;
}

bool Automaton::is_accept(int32_t state) const {
    if (state < 0 || static_cast<size_t>(state) >= is_accept_.size()) {
        return false;
    }
    return is_accept_[static_cast<size_t>(state)];
}

std::vector<std::vector<Transition>> Automaton::get_sorted_transitions() const {
    const int32_t num_states = get_num_states();
    std::vector<std::vector<Transition>> result;
    result.reserve(static_cast<size_t>(num_states));
    for (int32_t state = 0; state < num_states; ++state) {
        const int32_t count = get_num_transitions_with_state(state);
        std::vector<Transition> row;
        row.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i) {
            Transition transition;
            get_transition(state, i, transition);
            row.push_back(transition);
        }
        result.push_back(std::move(row));
    }
    return result;
}

VoidResult Automaton::add_transition_label(int32_t source, int32_t dest, int32_t label) {
    return add_transition(source, dest, label, label);
}

VoidResult Automaton::add_transition(int32_t source, int32_t dest, int32_t min, int32_t max) {
    debug_check(next_transition_ % 3 == 0, "transitions are stored three at a time");
    const auto bounds = static_cast<int32_t>(next_state_ / 2);
    debug_check(source >= 0 && source < bounds, "the source state must exist");
    debug_check(dest >= 0 && dest < bounds, "the destination state must exist");

    const auto source_index = static_cast<size_t>(source);
    if (!cur_state_.has_value() || cur_state_.value() != source_index) {
        if (cur_state_.has_value()) {
            if (auto finished = finish_current_state(cur_state_.value()); !finished) {
                return finished;
            }
        }
        cur_state_ = source_index;
        if (states_[2 * source_index] != -1) {
            return err(LuceneError::illegal_state(
                    std::format("from state ({}) already had transitions added", source)));
        }
        debug_check(states_[2 * source_index + 1] == 0, "a fresh state has no transitions");
        states_[2 * source_index] = static_cast<int32_t>(next_transition_);
    }

    // After `finish_current_state` the storage may have been truncated to
    // `next_transition_`, so grow it here rather than up front.
    grow_transitions();
    transitions_[next_transition_] = dest;
    transitions_[next_transition_ + 1] = min;
    transitions_[next_transition_ + 2] = max;
    next_transition_ += 3;
    states_[2 * source_index + 1] += 1;
    return {};
}

VoidResult Automaton::add_epsilon(int32_t source, int32_t dest) {
    Transition transition;
    const int32_t count = init_transition(dest, transition);
    for (int32_t i = 0; i < count; ++i) {
        if (auto next = get_next_transition(transition); !next) {
            return next;
        }
        if (auto added = add_transition(source, transition.dest, transition.min, transition.max); !added) {
            return added;
        }
    }
    if (is_accept(dest)) {
        set_accept(source, true);
    }
    return {};
}

VoidResult Automaton::copy(const Automaton &other) {
    if (&other == this) {
        return {};
    }
    const int32_t state_offset = get_num_states();
    const size_t total_states = next_state_ + other.next_state_;
    if (states_.size() < total_states) {
        states_.resize(total_states, 0);
    }
    std::copy(other.states_.begin(), other.states_.begin() + static_cast<std::ptrdiff_t>(other.next_state_),
              states_.begin() + static_cast<std::ptrdiff_t>(next_state_));

    const size_t copied_states_at = next_state_;
    for (size_t i = 0; i < other.next_state_; i += 2) {
        if (states_[copied_states_at + i] != -1) {
            states_[copied_states_at + i] += static_cast<int32_t>(next_transition_);
        }
    }
    next_state_ += other.next_state_;

    const int32_t other_num_states = other.get_num_states();
    for (int32_t state = 0; state < other_num_states; ++state) {
        if (other.is_accept(state)) {
            set_accept(state_offset + state, true);
        }
    }

    const size_t total_transitions = next_transition_ + other.next_transition_;
    if (transitions_.size() < total_transitions) {
        transitions_.resize(total_transitions, 0);
    }
    std::copy(other.transitions_.begin(),
              other.transitions_.begin() + static_cast<std::ptrdiff_t>(other.next_transition_),
              transitions_.begin() + static_cast<std::ptrdiff_t>(next_transition_));

    const size_t copied_transitions_at = next_transition_;
    for (size_t i = 0; i < other.next_transition_; i += 3) {
        transitions_[copied_transitions_at + i] += state_offset;
    }
    next_transition_ += other.next_transition_;

    if (!other.deterministic_) {
        deterministic_ = false;
    }
    return {};
}

VoidResult Automaton::finish_current_state(size_t state) {
    const int32_t num_transitions = states_[2 * state + 1];
    debug_check(num_transitions > 0, "a finished state has transitions");

    const auto offset = static_cast<size_t>(states_[2 * state]);
    std::vector<PackedTransition> packed;
    packed.reserve(static_cast<size_t>(num_transitions));
    for (int32_t i = 0; i < num_transitions; ++i) {
        const size_t base = offset + 3 * static_cast<size_t>(i);
        packed.push_back(PackedTransition{transitions_[base], transitions_[base + 1], transitions_[base + 2]});
    }
    // Sort by dest, then min, then max, so that transitions sharing a
    // destination become adjacent.
    std::stable_sort(packed.begin(), packed.end(), [](const PackedTransition &left, const PackedTransition &right) {
        if (left[0] != right[0]) {
            return left[0] < right[0];
        }
        if (left[1] != right[1]) {
            return left[1] < right[1];
        }
        return left[2] < right[2];
    });

    // Merge adjacent ranges that share a destination.
    size_t upto = 0;
    int32_t min = -1;
    int32_t max = -1;
    int32_t dest = -1;
    auto flush = [&](int32_t flush_dest, int32_t flush_min, int32_t flush_max) {
        transitions_[offset + 3 * upto] = flush_dest;
        transitions_[offset + 3 * upto + 1] = flush_min;
        transitions_[offset + 3 * upto + 2] = flush_max;
        ++upto;
    };
    for (const PackedTransition &transition : packed) {
        if (dest == transition[0]) {
            if (transition[1] <= max + 1) {
                if (transition[2] > max) {
                    max = transition[2];
                }
            } else {
                flush(dest, min, max);
                min = transition[1];
                max = transition[2];
            }
        } else {
            if (dest != -1) {
                flush(dest, min, max);
            }
            dest = transition[0];
            min = transition[1];
            max = transition[2];
        }
    }
    if (dest != -1) {
        flush(dest, min, max);
    }

    next_transition_ -= (static_cast<size_t>(num_transitions) - upto) * 3;
    transitions_.resize(next_transition_);
    states_[2 * state + 1] = static_cast<int32_t>(upto);

    // Sort by min, then max, then dest: the layout the lookup code expects.
    std::vector<PackedTransition> merged(upto);
    for (size_t i = 0; i < upto; ++i) {
        merged[i] = PackedTransition{transitions_[offset + 3 * i], transitions_[offset + 3 * i + 1],
                                     transitions_[offset + 3 * i + 2]};
    }
    std::stable_sort(merged.begin(), merged.end(), [](const PackedTransition &left, const PackedTransition &right) {
        if (left[1] != right[1]) {
            return left[1] < right[1];
        }
        if (left[2] != right[2]) {
            return left[2] < right[2];
        }
        return left[0] < right[0];
    });
    for (size_t i = 0; i < upto; ++i) {
        transitions_[offset + 3 * i] = merged[i][0];
        transitions_[offset + 3 * i + 1] = merged[i][1];
        transitions_[offset + 3 * i + 2] = merged[i][2];
    }

    if (deterministic_ && upto > 1) {
        int32_t last_max = transitions_[offset + 2];
        for (size_t i = 1; i < upto; ++i) {
            const int32_t next_min = transitions_[offset + 3 * i + 1];
            if (next_min <= last_max) {
                deterministic_ = false;
                break;
            }
            last_max = transitions_[offset + 3 * i + 2];
        }
    }
    return {};
}

VoidResult Automaton::finish_state() {
    if (cur_state_.has_value()) {
        const auto state = cur_state_.value();
        cur_state_.reset();
        return finish_current_state(state);
    }
    return {};
}

bool Automaton::transition_sorted(const Transition &t) const {
    if (!t.transition_upto.has_value()) {
        return false;
    }
    const size_t upto = t.transition_upto.value();
    // The transition is not initialized yet: it is the state's first one.
    if (static_cast<int32_t>(upto) == states_[2 * static_cast<size_t>(t.source)]) {
        return true;
    }
    const int32_t next_dest = transitions_[upto];
    const int32_t next_min = transitions_[upto + 1];
    const int32_t next_max = transitions_[upto + 2];
    if (next_min > t.min) {
        return true;
    }
    if (next_min < t.min) {
        return false;
    }
    if (next_max > t.max) {
        return true;
    }
    if (next_max < t.max) {
        return false;
    }
    return next_dest > t.dest;
}

std::vector<int32_t> Automaton::get_start_points() const {
    std::vector<int32_t> points;
    points.reserve(1 + 2 * (next_transition_ / 3));
    points.push_back(0);
    for (size_t state = 0; state < next_state_; state += 2) {
        const auto first = static_cast<size_t>(states_[state]);
        const size_t limit = first + 3 * static_cast<size_t>(states_[state + 1]);
        for (size_t transition = first; transition < limit; transition += 3) {
            const int32_t min = transitions_[transition + 1];
            const int32_t max = transitions_[transition + 2];
            points.push_back(min);
            if (max < MAX_CODE_POINT) {
                points.push_back(max + 1);
            }
        }
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
}

int32_t Automaton::step(int32_t state, int32_t label) const { return next_impl(state, 0, label, nullptr); }

int32_t Automaton::next(Transition &transition, int32_t label) const {
    const int32_t from = transition.transition_upto.has_value()
                                 ? static_cast<int32_t>(transition.transition_upto.value())
                                 : -1;
    return next_impl(transition.source, from, label, &transition);
}

int32_t Automaton::next_impl(int32_t state, int32_t from_transition_index, int32_t label,
                             Transition *transition) const {
    debug_check(label >= 0, "labels are non negative");

    const auto state_index = 2 * static_cast<size_t>(state);
    const int32_t first_transition = states_[state_index];
    const int32_t num_transitions = states_[state_index + 1];

    int32_t low = std::max(from_transition_index, 0);
    int32_t high = num_transitions - 1;
    // Transitions are sorted by min: binary search the one whose
    // [minLabel, maxLabel] contains `label`.
    while (low <= high) {
        const int32_t mid = static_cast<int32_t>(static_cast<uint32_t>(low + high) >> 1);
        const auto transition_index = static_cast<size_t>(first_transition + 3 * mid);
        const int32_t min_label = transitions_[transition_index + 1];
        if (min_label > label) {
            high = mid - 1;
        } else {
            const int32_t max_label = transitions_[transition_index + 2];
            if (max_label < label) {
                low = mid + 1;
            } else {
                const int32_t dest = transitions_[transition_index];
                if (transition != nullptr) {
                    transition->dest = dest;
                    transition->min = min_label;
                    transition->max = max_label;
                    transition->transition_upto = static_cast<size_t>(mid);
                }
                return dest;
            }
        }
    }

    if (transition != nullptr) {
        transition->dest = -1;
        transition->transition_upto = static_cast<size_t>(low);
    }
    return -1;
}

int32_t Automaton::init_transition(int32_t state, Transition &t) const {
    debug_check(state >= 0 && state < get_num_states(), "the state must exist");
    t.source = state;
    const int32_t transition_upto = states_[2 * static_cast<size_t>(state)];
    if (transition_upto == -1) {
        t.transition_upto.reset();
    } else {
        t.transition_upto = static_cast<size_t>(transition_upto);
    }
    return get_num_transitions_with_state(state);
}

VoidResult Automaton::get_next_transition(Transition &t) const {
    if (!t.transition_upto.has_value()) {
        return err(LuceneError::illegal_state("transition cursor is not initialized"));
    }
    const size_t base = t.transition_upto.value();
    const auto source_index = 2 * static_cast<size_t>(t.source);
    debug_check(static_cast<int64_t>(base) + 3 - states_[source_index] <= 3 * static_cast<int64_t>(states_[source_index + 1]),
                "the transition cursor must stay inside the state");
    debug_check(transition_sorted(t), "transitions are sorted");
    t.dest = transitions_[base];
    t.min = transitions_[base + 1];
    t.max = transitions_[base + 2];
    t.transition_upto = base + 3;
    return {};
}

int32_t Automaton::get_num_transitions_with_state(int32_t state) const {
    debug_check(state >= 0 && state < get_num_states(), "the state must exist");
    return states_[2 * static_cast<size_t>(state) + 1];
}

void Automaton::get_transition(int32_t state, int32_t index, Transition &t) const {
    const auto base = static_cast<size_t>(states_[2 * static_cast<size_t>(state)]);
    const size_t offset = base + 3 * static_cast<size_t>(index);
    t.source = state;
    t.dest = transitions_[offset];
    t.min = transitions_[offset + 1];
    t.max = transitions_[offset + 2];
}

bool Automaton::operator==(const Automaton &other) const {
    if (get_num_states() != other.get_num_states() || get_num_transitions() != other.get_num_transitions()) {
        return false;
    }
    const int32_t num_states = get_num_states();
    for (int32_t state = 0; state < num_states; ++state) {
        if (is_accept(state) != other.is_accept(state)) {
            return false;
        }
        const int32_t count = get_num_transitions_with_state(state);
        if (count != other.get_num_transitions_with_state(state)) {
            return false;
        }
        for (int32_t i = 0; i < count; ++i) {
            Transition left;
            Transition right;
            get_transition(state, i, left);
            other.get_transition(state, i, right);
            if (left.dest != right.dest || left.min != right.min || left.max != right.max) {
                return false;
            }
        }
    }
    return true;
}

std::string Automaton::to_dot() const {
    std::string out;
    const int32_t num_states = get_num_states();
    for (int32_t state = 0; state < num_states; ++state) {
        out += std::format("{}", state);
        if (is_accept(state)) {
            out += " [accept]";
        }
        out += ':';
        const int32_t count = get_num_transitions_with_state(state);
        for (int32_t i = 0; i < count; ++i) {
            Transition transition;
            get_transition(state, i, transition);
            out += std::format(" {}-{}->{}", transition.min, transition.max, transition.dest);
        }
        if (state + 1 < num_states) {
            out += '\n';
        }
    }
    return out;
}

AutomatonBuilder::AutomatonBuilder() {
    is_accept_.assign(16, false);
    transitions_.reserve(64);
}

AutomatonBuilder AutomatonBuilder::with_capacity(size_t num_states, size_t num_transitions) {
    AutomatonBuilder builder;
    builder.is_accept_.assign(num_states, false);
    builder.transitions_.reserve(num_transitions * 4);
    return builder;
}

VoidResult AutomatonBuilder::add_transition_label(int32_t source, int32_t dest, int32_t label) {
    return add_transition(source, dest, label, label);
}

VoidResult AutomatonBuilder::add_transition(int32_t source, int32_t dest, int32_t min, int32_t max) {
    const size_t new_len = next_transition_ + 4;
    if (transitions_.size() < new_len) {
        transitions_.resize(new_len, 0);
    }
    transitions_[next_transition_] = source;
    transitions_[next_transition_ + 1] = dest;
    transitions_[next_transition_ + 2] = min;
    transitions_[next_transition_ + 3] = max;
    next_transition_ = new_len;
    return {};
}

VoidResult AutomatonBuilder::add_epsilon(int32_t source, int32_t dest) {
    for (size_t upto = 0; upto < next_transition_; upto += 4) {
        if (transitions_[upto] == dest) {
            if (auto added = add_transition(source, transitions_[upto + 1], transitions_[upto + 2],
                                            transitions_[upto + 3]);
                !added) {
                return added;
            }
        }
    }
    if (is_accept(dest)) {
        set_accept(source, true);
    }
    return {};
}

Result<Automaton> AutomatonBuilder::finish() {
    const int32_t num_states = next_state_;
    const auto num_transitions = static_cast<int32_t>(next_transition_ / 4);
    Automaton automaton = Automaton::with_capacity(static_cast<size_t>(num_states),
                                                   static_cast<size_t>(num_transitions));

    for (int32_t state = 0; state < num_states; ++state) {
        (void)automaton.create_state();
        automaton.set_accept(state, is_accept(state));
    }

    // Sort by source, then min, then max, then dest: transitions must be added
    // state by state, in order.
    std::vector<std::array<int32_t, 4>> packed;
    packed.reserve(static_cast<size_t>(num_transitions));
    for (size_t upto = 0; upto < next_transition_; upto += 4) {
        packed.push_back(std::array<int32_t, 4>{transitions_[upto], transitions_[upto + 1], transitions_[upto + 2],
                                                transitions_[upto + 3]});
    }
    std::stable_sort(packed.begin(), packed.end(),
                     [](const std::array<int32_t, 4> &left, const std::array<int32_t, 4> &right) {
                         if (left[0] != right[0]) {
                             return left[0] < right[0];
                         }
                         if (left[2] != right[2]) {
                             return left[2] < right[2];
                         }
                         if (left[3] != right[3]) {
                             return left[3] < right[3];
                         }
                         return left[1] < right[1];
                     });
    for (const std::array<int32_t, 4> &transition : packed) {
        if (auto added = automaton.add_transition(transition[0], transition[1], transition[2], transition[3]);
            !added) {
            return err(std::move(added.error()));
        }
    }

    if (auto finished = automaton.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }
    return automaton;
}

int32_t AutomatonBuilder::create_state() {
    const int32_t state = next_state_;
    ++next_state_;
    if (is_accept_.size() <= static_cast<size_t>(state)) {
        is_accept_.resize(static_cast<size_t>(state) + 1, false);
    }
    return state;
}

void AutomatonBuilder::set_accept(int32_t state, bool accept) {
    debug_check(state >= 0 && state < next_state_, "the state must exist");
    if (is_accept_.size() <= static_cast<size_t>(state)) {
        is_accept_.resize(static_cast<size_t>(state) + 1, false);
    }
    is_accept_[static_cast<size_t>(state)] = accept;
}

bool AutomatonBuilder::is_accept(int32_t state) const {
    if (state < 0 || static_cast<size_t>(state) >= is_accept_.size()) {
        return false;
    }
    return is_accept_[static_cast<size_t>(state)];
}

VoidResult AutomatonBuilder::copy(const Automaton &other) {
    const int32_t offset = get_num_states();
    const int32_t other_num_states = other.get_num_states();

    copy_states(other);

    Transition transition;
    for (int32_t state = 0; state < other_num_states; ++state) {
        const int32_t count = other.init_transition(state, transition);
        for (int32_t i = 0; i < count; ++i) {
            if (auto next = other.get_next_transition(transition); !next) {
                return next;
            }
            if (auto added = add_transition(offset + state, offset + transition.dest, transition.min, transition.max);
                !added) {
                return added;
            }
        }
    }
    return {};
}

void AutomatonBuilder::copy_states(const Automaton &other) {
    const int32_t other_num_states = other.get_num_states();
    for (int32_t state = 0; state < other_num_states; ++state) {
        const int32_t new_state = create_state();
        set_accept(new_state, other.is_accept(state));
    }
}

} // namespace lucene
