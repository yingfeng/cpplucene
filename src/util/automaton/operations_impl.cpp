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

module lucene_core:operations.impl;

import :operations;

import :automaton;
import :bit_mixer;
import :bytes_ref;
import :bytes_ref_builder;
import :debug;
import :error;
import :fixed_bit_set;
import :ints_ref;
import :point_transition_set;
import :state_set;
import :string_helper;
import :transition;
import :types;

import std;

namespace lucene {

namespace {

/// A `(state of a1, state of a2)` pair, the key of intersection's work list.
struct StatePairKey {
    int32_t s1{0};
    int32_t s2{0};

    [[nodiscard]] bool operator==(const StatePairKey &other) const noexcept {
        return s1 == other.s1 && s2 == other.s2;
    }
};

struct StatePairKeyHash {
    [[nodiscard]] size_t operator()(const StatePairKey &key) const noexcept {
        return static_cast<size_t>(key.s1) * 31u + static_cast<size_t>(key.s2);
    }
};

/// The automaton accepting the empty language (`Automata::make_empty`).
///
/// The reference implementation's `Operations` calls `Automata` for these two automata, but
/// `Automata` itself needs `Operations` for its debug assertions, so the C++
/// port keeps the two constructions local to break the module cycle.
Automaton make_empty_language() {
    Automaton automaton;
    (void)automaton.finish_state();
    return automaton;
}

/// The automaton accepting only the empty string
/// (`Automata::make_empty_string`).
Automaton make_empty_string_language() {
    Automaton automaton;
    (void)automaton.create_state();
    automaton.set_accept(0, true);
    return automaton;
}

/// Reverses the active byte range in place.
void reverse_bytes(BytesRef &ref_bytes) {
    if (ref_bytes.length() <= 1) {
        return;
    }
    std::ranges::reverse(ref_bytes.as_mutable_slice());
}

} // namespace

Result<std::optional<Automaton>> Operations::determinize(const Automaton &a, size_t work_limit) {
    if (a.is_deterministic() || a.get_num_states() <= 1) {
        return std::optional<Automaton>{};
    }

    AutomatonBuilder builder;

    (void)builder.create_state();

    // All the frozen sets of this determinization: an automaton state ordinal
    // maps to a range of `state_values_`, and sets with the same hash are
    // chained through `next`.
    struct FrozenSet {
        size_t start{0};
        size_t end{0};
        std::optional<int32_t> next{};
    };
    std::vector<int32_t> state_values{0};
    std::vector<FrozenSet> state_sets{{0, 1, std::nullopt}};
    std::deque<int32_t> worklist;
    std::unordered_map<int64_t, int32_t> newstate;

    newstate.emplace(static_cast<int64_t>(BitMixer::mix_i32(0)) + 1, 0);
    worklist.push_back(0);
    builder.set_accept(0, a.is_accept(0));

    PointTransitionSet points;
    StateSet states_set(5);

    Transition transition;
    uint64_t effort_spent = 0;
    const auto effort_limit = static_cast<uint64_t>(work_limit) * 10;

    while (!worklist.empty()) {
        const int32_t r = worklist.front();
        worklist.pop_front();
        const FrozenSet frozen = state_sets[static_cast<size_t>(r)];
        effort_spent += frozen.end - frozen.start;
        if (effort_spent >= effort_limit) {
            return err(LuceneError::too_complex_to_determinize(std::format(
                    "Determinizing automaton with {} states and {} transitions would require more than {} effort",
                    a.get_num_states(), a.get_num_transitions(), work_limit)));
        }

        // Collate the outgoing transitions of every state in the set.
        for (size_t i = frozen.start; i < frozen.end; ++i) {
            const int32_t state = state_values[i];
            const int32_t num_transitions = a.get_num_transitions_with_state(state);
            (void)a.init_transition(state, transition);
            for (int32_t j = 0; j < num_transitions; ++j) {
                if (auto next = a.get_next_transition(transition); !next) {
                    return err(std::move(next.error()));
                }
                points.add(transition);
            }
        }

        if (points.count == 0) {
            continue;
        }

        points.sort();

        int32_t last_point = -1;
        int32_t acc_count = 0;

        for (size_t i = 0; i < points.count; ++i) {
            const int32_t point = points.points[i].point;
            if (states_set.size() > 0) {
                debug_check(last_point != -1, "a point follows another one");
                const int64_t hash = states_set.long_hash_code();
                const std::vector<int32_t> &values = states_set.get_array();
                const auto head_entry = newstate.find(hash);
                const std::optional<int32_t> head = head_entry == newstate.end() ? std::nullopt
                                                                                : std::optional<int32_t>(head_entry->second);
                std::optional<int32_t> existing = head;
                while (existing.has_value()) {
                    const FrozenSet &candidate = state_sets[static_cast<size_t>(existing.value())];
                    if (std::equal(values.begin(), values.end(), state_values.begin() +
                                                                     static_cast<std::ptrdiff_t>(candidate.start),
                                   state_values.begin() + static_cast<std::ptrdiff_t>(candidate.end))) {
                        break;
                    }
                    existing = candidate.next;
                }

                int32_t q = -1;
                if (existing.has_value()) {
                    q = existing.value();
                    debug_check((acc_count > 0) == builder.is_accept(q),
                                "an existing state agrees on acceptance");
                } else {
                    q = builder.create_state();
                    const size_t start = state_values.size();
                    state_values.insert(state_values.end(), values.begin(), values.end());
                    state_sets.push_back(FrozenSet{start, state_values.size(), head});
                    worklist.push_back(q);
                    builder.set_accept(q, acc_count > 0);
                    newstate.emplace(hash, q);
                }

                if (auto added = builder.add_transition(r, q, last_point, point - 1); !added) {
                    return err(std::move(added.error()));
                }
            }
            {
                PointTransitions &at_point = points.points[i];
                const TransitionList &ends = at_point.ends;
                for (size_t j = 0; j < ends.next; j += 3) {
                    const int32_t dest = ends.transitions[j];
                    if (auto decremented = states_set.decr(dest); !decremented) {
                        return err(std::move(decremented.error()));
                    }
                    if (a.is_accept(dest)) {
                        acc_count -= 1;
                    }
                }
                at_point.ends.next = 0;
            }
            {
                PointTransitions &at_point = points.points[i];
                const TransitionList &starts = at_point.starts;
                for (size_t j = 0; j < starts.next; j += 3) {
                    const int32_t dest = starts.transitions[j];
                    states_set.incr(dest);
                    if (a.is_accept(dest)) {
                        acc_count += 1;
                    }
                }
                last_point = point;
                at_point.starts.next = 0;
            }
        }
        points.reset();
        debug_check(states_set.size() == 0, "every state left the set");
    }

    auto result = builder.finish();
    if (!result) {
        return err(std::move(result.error()));
    }
    debug_check(result.value().is_deterministic(), "determinization yields a deterministic automaton");
    return std::optional<Automaton>(std::move(result.value()));
}

// ---------------------------------------------------------------------------
// construction from inputs
// ---------------------------------------------------------------------------

Result<Automaton> Operations::concatenate(const Automaton &a1, const Automaton &a2) {
    return concatenate_list(std::vector<const Automaton *>{&a1, &a2});
}

Result<Automaton> Operations::concatenate_list(const std::vector<const Automaton *> &automata) {
    Automaton result;
    // First pass: create all the states.
    for (const Automaton *a : automata) {
        if (a->get_num_states() == 0) {
            if (auto finished = result.finish_state(); !finished) {
                return err(std::move(finished.error()));
            }
            return result;
        }
        const int32_t num_states = a->get_num_states();
        for (int32_t i = 0; i < num_states; ++i) {
            (void)result.create_state();
        }
    }

    // Second pass: add the transitions, linking the accept states of each
    // automaton to the start of the next one.
    int32_t state_offset = 0;
    Transition t;
    for (size_t i = 0; i < automata.size(); ++i) {
        const Automaton &a = *automata[i];
        const int32_t num_states = a.get_num_states();
        const Automaton *next_a = (i + 1 < automata.size()) ? automata[i + 1] : nullptr;

        for (int32_t s = 0; s < num_states; ++s) {
            const int32_t count = a.init_transition(s, t);
            for (int32_t j = 0; j < count; ++j) {
                if (auto next = a.get_next_transition(t); !next) {
                    return err(std::move(next.error()));
                }
                if (auto added = result.add_transition(state_offset + s, state_offset + t.dest, t.min, t.max); !added) {
                    return err(std::move(added.error()));
                }
            }

            if (a.is_accept(s)) {
                int32_t follow_offset = state_offset;
                size_t upto = i + 1;
                const Automaton *follow_a = next_a;

                while (true) {
                    if (follow_a == nullptr) {
                        result.set_accept(state_offset + s, true);
                        break;
                    }
                    const int32_t num_transitions = follow_a->init_transition(0, t);
                    for (int32_t j = 0; j < num_transitions; ++j) {
                        if (auto next = follow_a->get_next_transition(t); !next) {
                            return err(std::move(next.error()));
                        }
                        if (auto added = result.add_transition(state_offset + s, follow_offset + num_states + t.dest,
                                                               t.min, t.max);
                            !added) {
                            return err(std::move(added.error()));
                        }
                    }
                    if (!follow_a->is_accept(0)) {
                        break;
                    }
                    follow_offset += follow_a->get_num_states();
                    follow_a = (upto + 1 < automata.size()) ? automata[upto + 1] : nullptr;
                    upto += 1;
                }
            }
        }

        state_offset += num_states;
    }

    if (result.get_num_states() == 0) {
        (void)result.create_state();
    }
    if (auto finished = result.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }
    return result;
}

Result<Automaton> Operations::union_with(const Automaton &a1, const Automaton &a2) {
    return union_list(std::vector<const Automaton *>{&a1, &a2});
}

Result<Automaton> Operations::union_list(const std::vector<const Automaton *> &automata) {
    Automaton result;
    (void)result.create_state();

    for (const Automaton *a : automata) {
        if (auto copied = result.copy(*a); !copied) {
            return err(std::move(copied.error()));
        }
    }

    // Epsilon transitions from the new initial state.
    int32_t state_offset = 1;
    for (const Automaton *a : automata) {
        if (a->get_num_states() == 0) {
            continue;
        }
        if (auto added = result.add_epsilon(0, state_offset); !added) {
            return err(std::move(added.error()));
        }
        state_offset += a->get_num_states();
    }

    if (auto finished = result.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }

    auto reduced = remove_dead_states(result);
    if (!reduced) {
        return err(std::move(reduced.error()));
    }
    return reduced.value().has_value() ? std::move(reduced.value().value()) : std::move(result);
}

// ---------------------------------------------------------------------------
// closures
// ---------------------------------------------------------------------------

Result<std::optional<Automaton>> Operations::optional(const Automaton &a) {
    if (a.is_accept(0)) {
        return std::optional<Automaton>{};
    }

    bool has_transitions_to_initial = false;
    Transition t;
    for (int32_t s = 0; s < a.get_num_states() && !has_transitions_to_initial; ++s) {
        const int32_t count = a.init_transition(s, t);
        for (int32_t j = 0; j < count; ++j) {
            if (auto next = a.get_next_transition(t); !next) {
                return err(std::move(next.error()));
            }
            if (t.dest == 0) {
                has_transitions_to_initial = true;
                break;
            }
        }
    }

    if (!has_transitions_to_initial) {
        Automaton result;
        if (auto copied = result.copy(a); !copied) {
            return err(std::move(copied.error()));
        }
        if (result.get_num_states() == 0) {
            (void)result.create_state();
        }
        result.set_accept(0, true);
        return std::optional<Automaton>(std::move(result));
    }

    Automaton result;
    (void)result.create_state();
    result.set_accept(0, true);
    if (a.get_num_states() > 0) {
        if (auto copied = result.copy(a); !copied) {
            return err(std::move(copied.error()));
        }
        if (auto added = result.add_epsilon(0, 1); !added) {
            return err(std::move(added.error()));
        }
    }
    if (auto finished = result.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }
    return std::optional<Automaton>(std::move(result));
}

Result<std::optional<Automaton>> Operations::repeat(const Automaton &a) {
    if (a.get_num_states() == 0) {
        // Repeating the empty automaton still only accepts nothing.
        return std::optional<Automaton>{};
    }

    // State 0 is the only accept state: it already repeats itself.
    if (a.is_accept(0)) {
        int32_t accept_count = 0;
        for (int32_t i = 0; i < a.get_num_states(); ++i) {
            if (a.is_accept(i)) {
                ++accept_count;
            }
        }
        if (accept_count == 1) {
            return std::optional<Automaton>{};
        }
    }

    AutomatonBuilder builder;
    (void)builder.create_state(); // initial state
    builder.set_accept(0, true);

    const int32_t num_states = a.get_num_states();
    std::vector<int32_t> state_map(static_cast<size_t>(num_states), 0);
    for (int32_t state = 0; state < num_states; ++state) {
        const auto state_index = static_cast<size_t>(state);
        if (!a.is_accept(state)) {
            state_map[state_index] = builder.create_state();
        } else if (a.get_num_transitions_with_state(state) == 0) {
            state_map[state_index] = 0; // merge into the initial state
        } else {
            const int32_t new_state = builder.create_state();
            state_map[state_index] = new_state;
            builder.set_accept(new_state, true);
        }
    }

    Transition t;
    for (int32_t state = 0; state < num_states; ++state) {
        const int32_t src = state_map[static_cast<size_t>(state)];
        const int32_t count = a.init_transition(state, t);
        for (int32_t j = 0; j < count; ++j) {
            if (auto next = a.get_next_transition(t); !next) {
                return err(std::move(next.error()));
            }
            const int32_t dest = state_map[static_cast<size_t>(t.dest)];
            if (auto added = builder.add_transition(src, dest, t.min, t.max); !added) {
                return err(std::move(added.error()));
            }
        }
    }

    // The initial transitions also leave the new initial state.
    const int32_t initial_count = a.init_transition(0, t);
    for (int32_t j = 0; j < initial_count; ++j) {
        if (auto next = a.get_next_transition(t); !next) {
            return err(std::move(next.error()));
        }
        if (auto added = builder.add_transition(0, state_map[static_cast<size_t>(t.dest)], t.min, t.max); !added) {
            return err(std::move(added.error()));
        }
    }

    // Every accept state repeats the initial transitions.
    for (int32_t s = 0; s < num_states; ++s) {
        if (!a.is_accept(s) || state_map[static_cast<size_t>(s)] == 0) {
            continue;
        }
        const int32_t count = a.init_transition(0, t);
        for (int32_t j = 0; j < count; ++j) {
            if (auto next = a.get_next_transition(t); !next) {
                return err(std::move(next.error()));
            }
            if (auto added = builder.add_transition(state_map[static_cast<size_t>(s)],
                                                    state_map[static_cast<size_t>(t.dest)], t.min, t.max);
                !added) {
                return err(std::move(added.error()));
            }
        }
    }

    auto automaton = builder.finish();
    if (!automaton) {
        return err(std::move(automaton.error()));
    }

    auto reduced = remove_dead_states(automaton.value());
    if (!reduced) {
        return err(std::move(reduced.error()));
    }
    return reduced.value().has_value() ? std::optional<Automaton>(std::move(reduced.value().value()))
                                       : std::optional<Automaton>(std::move(automaton.value()));
}

Result<std::optional<Automaton>> Operations::repeat_count(const Automaton &a, int32_t count) {
    if (count == 0) {
        return repeat(a);
    }

    std::vector<const Automaton *> automata;
    automata.reserve(static_cast<size_t>(count) + 1);
    for (int32_t i = 0; i < count; ++i) {
        automata.push_back(&a);
    }
    auto repeated = repeat(a);
    if (!repeated) {
        return err(std::move(repeated.error()));
    }
    const Automaton &tail = repeated.value().has_value() ? repeated.value().value() : a;
    automata.push_back(&tail);

    auto concatenated = concatenate_list(automata);
    if (!concatenated) {
        return err(std::move(concatenated.error()));
    }
    return std::optional<Automaton>(std::move(concatenated.value()));
}

Result<Automaton> Operations::repeat_min_max(const Automaton &a, int32_t min, int32_t max) {
    if (min > max) {
        return make_empty_language();
    }

    Automaton b;
    if (min == 0) {
        b = make_empty_string_language();
    } else if (min == 1) {
        if (auto copied = b.copy(a); !copied) {
            return err(std::move(copied.error()));
        }
    } else {
        std::vector<const Automaton *> reps(static_cast<size_t>(min), &a);
        auto concatenated = concatenate_list(reps);
        if (!concatenated) {
            return err(std::move(concatenated.error()));
        }
        b = std::move(concatenated.value());
    }

    std::set<int32_t> prev_accept = get_accept_states(b, 0);
    AutomatonBuilder builder;
    if (auto copied = builder.copy(b); !copied) {
        return err(std::move(copied.error()));
    }

    for (int32_t i = min; i < max; ++i) {
        const int32_t offset = builder.get_num_states();
        if (auto copied = builder.copy(a); !copied) {
            return err(std::move(copied.error()));
        }
        for (const int32_t s : prev_accept) {
            if (auto added = builder.add_epsilon(s, offset); !added) {
                return err(std::move(added.error()));
            }
        }
        prev_accept = get_accept_states(a, offset);
    }

    return builder.finish();
}

std::set<int32_t> Operations::get_accept_states(const Automaton &a, int32_t offset) {
    std::set<int32_t> result;
    for (int32_t s = 0; s < a.get_num_states(); ++s) {
        if (a.is_accept(s)) {
            result.insert(offset + s);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// boolean algebra
// ---------------------------------------------------------------------------

Result<Automaton> Operations::complement(const Automaton &a, size_t determinize_work_limit) {
    auto deterministic = determinize(a, determinize_work_limit);
    if (!deterministic) {
        return err(std::move(deterministic.error()));
    }
    const Automaton &base = deterministic.value().has_value() ? deterministic.value().value() : a;

    auto total = totalize(base);
    if (!total) {
        return err(std::move(total.error()));
    }
    Automaton &result = total.value();

    const int32_t num_states = result.get_num_states();
    for (int32_t p = 0; p < num_states; ++p) {
        result.set_accept(p, !result.is_accept(p));
    }

    auto reduced = remove_dead_states(result);
    if (!reduced) {
        return err(std::move(reduced.error()));
    }
    return reduced.value().has_value() ? std::move(reduced.value().value()) : std::move(result);
}

Result<std::optional<Automaton>> Operations::minus(const Automaton &a1, const Automaton &a2,
                                                   size_t determinize_work_limit) {
    auto empty1 = is_empty(a1);
    if (!empty1) {
        return err(std::move(empty1.error()));
    }
    if (empty1.value() || &a1 == &a2) {
        return std::optional<Automaton>(make_empty_language());
    }
    auto empty2 = is_empty(a2);
    if (!empty2) {
        return err(std::move(empty2.error()));
    }
    if (empty2.value()) {
        return std::optional<Automaton>{};
    }

    auto complement_a2 = complement(a2, determinize_work_limit);
    if (!complement_a2) {
        return err(std::move(complement_a2.error()));
    }

    // `nullopt` means "the first argument", which is what the reference implementation's
    // `Cow::Borrowed(a1)` arm returns; the second borrowed arm of the Rust
    // version is materialised by `intersection` itself.
    auto common = intersection(a1, complement_a2.value());
    if (!common) {
        return err(std::move(common.error()));
    }
    return common;
}

Result<std::optional<Automaton>> Operations::intersection(const Automaton &a1, const Automaton &a2) {
    if (&a1 == &a2) {
        return std::optional<Automaton>{};
    }
    if (a1.get_num_states() == 0) {
        return std::optional<Automaton>{};
    }
    if (a2.get_num_states() == 0) {
        // The reference implementation borrows the second automaton here; `nullopt` means "the first
        // one", so the (state free) result is materialised instead.
        return std::optional<Automaton>(Automaton());
    }

    const std::vector<std::vector<Transition>> transitions1 = a1.get_sorted_transitions();
    const std::vector<std::vector<Transition>> transitions2 = a2.get_sorted_transitions();

    Automaton c;
    (void)c.create_state();

    std::deque<StatePairKey> worklist;
    std::unordered_map<StatePairKey, int32_t, StatePairKeyHash> newstates;

    const StatePairKey first{0, 0};
    worklist.push_back(first);
    newstates.emplace(first, 0);

    while (!worklist.empty()) {
        const StatePairKey p = worklist.front();
        worklist.pop_front();
        c.set_accept(newstates.at(p), a1.is_accept(p.s1) && a2.is_accept(p.s2));

        const std::vector<Transition> &t1 = transitions1[static_cast<size_t>(p.s1)];
        const std::vector<Transition> &t2 = transitions2[static_cast<size_t>(p.s2)];

        size_t n1 = 0;
        size_t b2 = 0;

        while (n1 < t1.size()) {
            while (b2 < t2.size() && t2[b2].max < t1[n1].min) {
                b2 += 1;
            }
            size_t n2 = b2;
            while (n2 < t2.size() && t1[n1].max >= t2[n2].min) {
                if (t2[n2].max >= t1[n1].min) {
                    const StatePairKey q{t1[n1].dest, t2[n2].dest};
                    const auto found = newstates.find(q);
                    int32_t r = -1;
                    if (found != newstates.end()) {
                        r = found->second;
                    } else {
                        r = c.create_state();
                        worklist.push_back(q);
                        newstates.emplace(q, r);
                    }

                    const int32_t min = std::max(t1[n1].min, t2[n2].min);
                    const int32_t max = std::min(t1[n1].max, t2[n2].max);

                    if (auto added = c.add_transition(newstates.at(p), r, min, max); !added) {
                        return err(std::move(added.error()));
                    }
                }
                n2 += 1;
            }
            n1 += 1;
        }
    }

    if (auto finished = c.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }

    auto reduced = remove_dead_states(c);
    if (!reduced) {
        return err(std::move(reduced.error()));
    }
    return reduced.value().has_value() ? std::optional<Automaton>(std::move(reduced.value().value()))
                                       : std::optional<Automaton>(std::move(c));
}

// ---------------------------------------------------------------------------
// inspection
// ---------------------------------------------------------------------------

Result<bool> Operations::has_dead_states(const Automaton &a) {
    auto live = get_live_states(a);
    if (!live) {
        return err(std::move(live.error()));
    }
    const size_t num_live = live.value().cardinality();
    const auto num_states = static_cast<size_t>(a.get_num_states());
    debug_check(num_live <= num_states, "a live state is a state");
    return num_live < num_states;
}

Result<bool> Operations::has_dead_states_from_initial(const Automaton &a) {
    auto reachable_from_initial = get_live_states_from_initial(a);
    if (!reachable_from_initial) {
        return err(std::move(reachable_from_initial.error()));
    }
    auto reachable_from_accept = get_live_states_to_accept(a);
    if (!reachable_from_accept) {
        return err(std::move(reachable_from_accept.error()));
    }
    reachable_from_initial.value().and_not(reachable_from_accept.value());
    return reachable_from_initial.value().cardinality() != 0;
}

Result<bool> Operations::has_dead_states_to_accept(const Automaton &a) {
    auto reachable_from_initial = get_live_states_from_initial(a);
    if (!reachable_from_initial) {
        return err(std::move(reachable_from_initial.error()));
    }
    auto reachable_from_accept = get_live_states_to_accept(a);
    if (!reachable_from_accept) {
        return err(std::move(reachable_from_accept.error()));
    }
    reachable_from_accept.value().and_not(reachable_from_initial.value());
    return reachable_from_accept.value().cardinality() != 0;
}

Result<bool> Operations::is_empty(const Automaton &a) {
    if (a.get_num_states() == 0) {
        return true;
    }
    if (!a.is_accept(0) && a.get_num_transitions_with_state(0) == 0) {
        // Common case: just one initial state.
        return true;
    }
    if (a.is_accept(0)) {
        // Apparently common case: it accepts the empty string.
        return false;
    }

    std::deque<int32_t> work_list;
    FixedBitSet seen(static_cast<size_t>(a.get_num_states()));
    work_list.push_back(0);
    seen.set(0);

    Transition t;
    while (!work_list.empty()) {
        const int32_t state = work_list.front();
        work_list.pop_front();
        if (a.is_accept(state)) {
            return false;
        }

        const int32_t count = a.init_transition(state, t);
        for (int32_t j = 0; j < count; ++j) {
            if (auto next = a.get_next_transition(t); !next) {
                return err(std::move(next.error()));
            }
            if (seen.set(static_cast<size_t>(t.dest))) {
                work_list.push_back(t.dest);
            }
        }
    }

    return true;
}

Result<bool> Operations::is_total(const Automaton &a) {
    return is_total_with_range(a, 0, MAX_CODE_POINT);
}

Result<bool> Operations::is_total_with_range(const Automaton &a, int32_t min_alphabet, int32_t max_alphabet) {
    auto live = get_live_states(a);
    if (!live) {
        return err(std::move(live.error()));
    }
    const FixedBitSet &states = live.value();

    Transition spare;
    int32_t seen_states = 0;

    int64_t state = states.next_set_bit(0);
    while (state >= 0) {
        const auto state_id = static_cast<int32_t>(state);
        if (!a.is_accept(state_id)) {
            return false;
        }
        int32_t previous_label = min_alphabet - 1;
        const int32_t count = a.get_num_transitions_with_state(state_id);
        for (int32_t i = 0; i < count; ++i) {
            a.get_transition(state_id, i, spare);
            if (spare.min > previous_label + 1) {
                return false;
            }
            previous_label = spare.max;
        }

        if (previous_label < max_alphabet) {
            return false;
        }

        if (state_id == std::numeric_limits<int32_t>::max()) {
            break;
        }

        seen_states += 1;
        state = states.next_set_bit(static_cast<size_t>(state_id) + 1);
    }
    return seen_states > 0;
}

bool Operations::run_str(const Automaton &a, std::string_view s) {
    debug_check(a.is_deterministic(), "the input must be deterministic");

    int32_t state = 0;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t num_bytes = 0;
        auto code_point = StringHelper::utf8_code_point_at(s, pos, num_bytes);
        if (!code_point) {
            debug_check(false, "the input must be valid UTF-8");
            return false;
        }
        const int32_t next_state = a.step(state, code_point.value());
        if (next_state == -1) {
            return false;
        }
        state = next_state;
        pos += num_bytes;
    }

    return a.is_accept(state);
}

bool Operations::run_ints_ref(const Automaton &a, const IntsRef &s) {
    debug_check(a.is_deterministic(), "the input must be deterministic");

    int32_t state = 0;
    for (const int32_t label : s.as_slice()) {
        const int32_t next_state = a.step(state, label);
        if (next_state == -1) {
            return false;
        }
        state = next_state;
    }
    return a.is_accept(state);
}

// ---------------------------------------------------------------------------
// live states
// ---------------------------------------------------------------------------

Result<FixedBitSet> Operations::get_live_states(const Automaton &a) {
    auto reachable = get_live_states_from_initial(a);
    if (!reachable) {
        return err(std::move(reachable.error()));
    }
    auto productive = get_live_states_to_accept(a);
    if (!productive) {
        return err(std::move(productive.error()));
    }
    reachable.value().and_with(productive.value());
    return reachable;
}

Result<FixedBitSet> Operations::get_live_states_from_initial(const Automaton &a) {
    const int32_t num_states = a.get_num_states();
    FixedBitSet live(static_cast<size_t>(num_states));
    if (num_states == 0) {
        return live;
    }

    std::deque<int32_t> work_list;
    live.set(0);
    work_list.push_back(0);

    Transition t;
    while (!work_list.empty()) {
        const int32_t s = work_list.front();
        work_list.pop_front();
        const int32_t count = a.init_transition(s, t);
        for (int32_t j = 0; j < count; ++j) {
            if (auto next = a.get_next_transition(t); !next) {
                return err(std::move(next.error()));
            }
            if (live.set(static_cast<size_t>(t.dest))) {
                work_list.push_back(t.dest);
            }
        }
    }
    return live;
}

Result<FixedBitSet> Operations::get_live_states_to_accept(const Automaton &a) {
    const int32_t num_states = a.get_num_states();

    // Build the reversed automaton.
    AutomatonBuilder builder;
    for (int32_t i = 0; i < num_states; ++i) {
        (void)builder.create_state();
    }
    Transition t;
    for (int32_t s = 0; s < num_states; ++s) {
        const int32_t count = a.init_transition(s, t);
        for (int32_t j = 0; j < count; ++j) {
            if (auto next = a.get_next_transition(t); !next) {
                return err(std::move(next.error()));
            }
            if (auto added = builder.add_transition(t.dest, s, t.min, t.max); !added) {
                return err(std::move(added.error()));
            }
        }
    }
    auto reversed = builder.finish();
    if (!reversed) {
        return err(std::move(reversed.error()));
    }

    // Start from the accept states and walk the reversed transitions.
    FixedBitSet live(static_cast<size_t>(num_states));
    std::deque<int32_t> work_list;
    for (int32_t s = 0; s < num_states; ++s) {
        if (a.is_accept(s)) {
            live.set(static_cast<size_t>(s));
            work_list.push_back(s);
        }
    }
    while (!work_list.empty()) {
        const int32_t s = work_list.front();
        work_list.pop_front();
        const int32_t count = reversed.value().init_transition(s, t);
        for (int32_t j = 0; j < count; ++j) {
            if (auto next = reversed.value().get_next_transition(t); !next) {
                return err(std::move(next.error()));
            }
            if (live.set(static_cast<size_t>(t.dest))) {
                work_list.push_back(t.dest);
            }
        }
    }
    return live;
}

Result<std::optional<Automaton>> Operations::remove_dead_states(const Automaton &a) {
    const int32_t num_states = a.get_num_states();
    auto live = get_live_states(a);
    if (!live) {
        return err(std::move(live.error()));
    }
    const FixedBitSet &live_set = live.value();
    if (live_set.cardinality() == static_cast<size_t>(num_states)) {
        return std::optional<Automaton>{};
    }

    std::vector<int32_t> map(static_cast<size_t>(num_states), 0);
    Automaton result;

    for (int32_t i = 0; i < num_states; ++i) {
        if (live_set.get_bit(static_cast<size_t>(i))) {
            const int32_t s = result.create_state();
            map[static_cast<size_t>(i)] = s;
            result.set_accept(s, a.is_accept(i));
        }
    }

    Transition t;
    for (int32_t i = 0; i < num_states; ++i) {
        if (!live_set.get_bit(static_cast<size_t>(i))) {
            continue;
        }
        const int32_t num_transitions = a.init_transition(i, t);
        for (int32_t j = 0; j < num_transitions; ++j) {
            if (auto next = a.get_next_transition(t); !next) {
                return err(std::move(next.error()));
            }
            if (live_set.get_bit(static_cast<size_t>(t.dest))) {
                if (auto added = result.add_transition(map[static_cast<size_t>(i)],
                                                       map[static_cast<size_t>(t.dest)], t.min, t.max);
                    !added) {
                    return err(std::move(added.error()));
                }
            }
        }
    }

    if (auto finished = result.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }
    debug_check(!has_dead_states(result).value_or(true), "the result has no dead states");
    return std::optional<Automaton>(std::move(result));
}

Result<Automaton> Operations::totalize(const Automaton &a) {
    Automaton result;

    const int32_t num_states = a.get_num_states();
    for (int32_t i = 0; i < num_states; ++i) {
        (void)result.create_state();
        result.set_accept(i, a.is_accept(i));
    }

    const int32_t dead_state = result.create_state();
    if (auto added = result.add_transition(dead_state, dead_state, 0, MAX_CODE_POINT); !added) {
        return err(std::move(added.error()));
    }

    Transition t;
    for (int32_t i = 0; i < num_states; ++i) {
        int32_t maxi = 0;
        const int32_t count = a.init_transition(i, t);

        for (int32_t j = 0; j < count; ++j) {
            if (auto next = a.get_next_transition(t); !next) {
                return err(std::move(next.error()));
            }
            if (auto added = result.add_transition(i, t.dest, t.min, t.max); !added) {
                return err(std::move(added.error()));
            }
            if (t.min > maxi) {
                if (auto added = result.add_transition(i, dead_state, maxi, t.min - 1); !added) {
                    return err(std::move(added.error()));
                }
            }
            if (t.max + 1 > maxi) {
                maxi = t.max + 1;
            }
        }

        if (maxi <= MAX_CODE_POINT) {
            if (auto added = result.add_transition(i, dead_state, maxi, MAX_CODE_POINT); !added) {
                return err(std::move(added.error()));
            }
        }
    }

    if (auto finished = result.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }
    return result;
}

// ---------------------------------------------------------------------------
// common prefixes and suffixes
// ---------------------------------------------------------------------------

Result<std::string> Operations::get_common_prefix(const Automaton &a) {
    auto dead = has_dead_states_from_initial(a);
    if (!dead) {
        return err(std::move(dead.error()));
    }
    if (dead.value()) {
        return err(LuceneError::illegal_argument("input automaton has dead states"));
    }

    auto empty = is_empty(a);
    if (!empty) {
        return err(std::move(empty.error()));
    }
    if (empty.value()) {
        return std::string();
    }

    std::string builder;
    Transition scratch;
    const auto capacity = static_cast<size_t>(a.get_num_states());
    FixedBitSet current(capacity);
    FixedBitSet next(capacity);
    current.set(0); // start with the initial state

    while (true) {
        int32_t label = -1;
        bool stop = false;
        int64_t state = current.next_set_bit(0);
        // One pass: step every current path forward once.
        while (state >= 0) {
            const auto state_id = static_cast<int32_t>(state);
            if (a.is_accept(state_id)) {
                stop = true;
                break;
            }

            const int32_t num_transitions = a.get_num_transitions_with_state(state_id);
            for (int32_t t_idx = 0; t_idx < num_transitions; ++t_idx) {
                a.get_transition(state_id, t_idx, scratch);
                if (label == -1) {
                    label = scratch.min;
                }
                // Either a range of labels, or a label that does not match all
                // the other paths this round.
                if (scratch.min != scratch.max || scratch.min != label) {
                    stop = true;
                    break;
                }
                next.set(static_cast<size_t>(scratch.dest));
            }
            if (stop) {
                break;
            }

            if (state + 1 >= static_cast<int64_t>(current.length())) {
                break;
            }
            state = current.next_set_bit(static_cast<size_t>(state + 1));
        }

        if (stop) {
            break;
        }

        debug_check(label != -1, "no dead end states were checked up front");
        if (auto appended = StringHelper::utf8_append(builder, label); !appended) {
            return err(std::move(appended.error()));
        }

        std::swap(current, next);
        next.clear_all();
    }

    return builder;
}

Result<BytesRef> Operations::get_common_prefix_bytes_ref(const Automaton &a) {
    auto prefix = get_common_prefix(a);
    if (!prefix) {
        return err(std::move(prefix.error()));
    }
    auto code_points = StringHelper::utf8_to_code_points(prefix.value());
    if (!code_points) {
        return err(std::move(code_points.error()));
    }

    BytesRefBuilder builder;
    for (const int32_t code_point : code_points.value()) {
        if (code_point > 255) {
            return err(LuceneError::illegal_state("automaton is not binary"));
        }
        if (auto appended = builder.append_byte(static_cast<uint8_t>(code_point)); !appended) {
            return err(std::move(appended.error()));
        }
    }
    return builder.get_bytes_owner();
}

Result<BytesRef> Operations::get_common_suffix_bytes_ref(const Automaton &a) {
    auto reversed = reverse(a);
    if (!reversed) {
        return err(std::move(reversed.error()));
    }
    auto reduced = remove_dead_states(reversed.value());
    if (!reduced) {
        return err(std::move(reduced.error()));
    }
    const Automaton &minimal = reduced.value().has_value() ? reduced.value().value() : reversed.value();

    auto bytes_ref = get_common_prefix_bytes_ref(minimal);
    if (!bytes_ref) {
        return err(std::move(bytes_ref.error()));
    }
    reverse_bytes(bytes_ref.value());
    return std::move(bytes_ref.value());
}

Result<std::optional<IntsRef>> Operations::get_singleton(const Automaton &a) {
    if (!a.is_deterministic()) {
        return err(LuceneError::illegal_argument("input automaton must be deterministic"));
    }

    IntsRefBuilder builder;
    std::unordered_set<int32_t> visited;
    int32_t s = 0;
    Transition t;
    while (true) {
        visited.insert(s);
        if (!a.is_accept(s)) {
            if (a.get_num_transitions_with_state(s) == 1) {
                a.get_transition(s, 0, t);
                if (t.min == t.max && visited.find(t.dest) == visited.end()) {
                    if (auto appended = builder.append(t.min); !appended) {
                        return err(std::move(appended.error()));
                    }
                    s = t.dest;
                    continue;
                }
            }
        } else if (a.get_num_transitions_with_state(s) == 0) {
            return std::optional<IntsRef>(builder.to_ints_ref());
        }

        // The automaton accepts more than one string.
        return std::optional<IntsRef>{};
    }
}

// ---------------------------------------------------------------------------
// reversal
// ---------------------------------------------------------------------------

Result<Automaton> Operations::reverse(const Automaton &a) {
    return reverse_with_initial_states(a, nullptr);
}

Result<Automaton> Operations::reverse_with_initial_states(const Automaton &a, std::set<int32_t> *initial_states) {
    auto empty = is_empty(a);
    if (!empty) {
        return err(std::move(empty.error()));
    }
    if (empty.value()) {
        return Automaton();
    }

    const int32_t num_states = a.get_num_states();

    AutomatonBuilder builder;
    (void)builder.create_state(); // new initial state
    for (int32_t i = 0; i < num_states; ++i) {
        (void)builder.create_state();
    }
    // The old initial state (0) becomes accept state (index 1).
    builder.set_accept(1, true);

    Transition t;
    for (int32_t s = 0; s < num_states; ++s) {
        const int32_t num_transitions = a.get_num_transitions_with_state(s);
        (void)a.init_transition(s, t);
        for (int32_t j = 0; j < num_transitions; ++j) {
            if (auto next = a.get_next_transition(t); !next) {
                return err(std::move(next.error()));
            }
            if (auto added = builder.add_transition(t.dest + 1, s + 1, t.min, t.max); !added) {
                return err(std::move(added.error()));
            }
        }
    }

    auto finished = builder.finish();
    if (!finished) {
        return err(std::move(finished.error()));
    }
    Automaton result = std::move(finished.value());

    for (int32_t s = 0; s < num_states; ++s) {
        if (!a.is_accept(s)) {
            continue;
        }
        if (auto added = result.add_epsilon(0, s + 1); !added) {
            return err(std::move(added.error()));
        }
        if (initial_states != nullptr) {
            initial_states->insert(s + 1);
        }
    }

    if (auto finished_result = result.finish_state(); !finished_result) {
        return err(std::move(finished_result.error()));
    }
    return result;
}

// ---------------------------------------------------------------------------
// ordering
// ---------------------------------------------------------------------------

Result<std::vector<int32_t>> Operations::topo_sort_states(const Automaton &a) {
    if (a.get_num_states() == 0) {
        return std::vector<int32_t>{};
    }

    std::vector<int32_t> states(static_cast<size_t>(a.get_num_states()), 0);
    auto upto = topo_sort_states_with_state(a, states);
    if (!upto) {
        return err(std::move(upto.error()));
    }
    states.resize(upto.value());
    std::ranges::reverse(states);
    return states;
}

Result<size_t> Operations::topo_sort_states_with_state(const Automaton &a, std::span<int32_t> states) {
    const auto num_states = static_cast<size_t>(a.get_num_states());
    FixedBitSet on_stack(num_states);
    FixedBitSet visited(num_states);
    std::vector<int32_t> stack;
    stack.push_back(0); // the initial state is 0
    size_t upto = 0;

    Transition t;
    while (!stack.empty()) {
        const int32_t state = stack.back();
        const auto state_index = static_cast<size_t>(state);
        const int32_t count = a.init_transition(state, t);
        bool pushed = false;

        for (int32_t j = 0; j < count; ++j) {
            if (auto next = a.get_next_transition(t); !next) {
                return err(std::move(next.error()));
            }
            const auto dest_index = static_cast<size_t>(t.dest);
            if (visited.set(dest_index)) {
                stack.push_back(t.dest);
                on_stack.set(state_index);
                pushed = true;
                break;
            }
            if (on_stack.get_bit(dest_index)) {
                return err(LuceneError::illegal_argument("input automaton has a cycle"));
            }
        }

        if (!pushed) {
            // Done with this state: leave the recursion stack.
            on_stack.clear(state_index);
            stack.pop_back();
            states[upto] = state;
            upto += 1;
        }
    }
    return upto;
}

} // namespace lucene
