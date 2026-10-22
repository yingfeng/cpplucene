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

module lucene_core:automata.impl;

import :automata;

import :automaton;
import :bytes_ref;
import :bytes_ref_iterator;
import :debug;
import :error;
import :operations;
import :string_helper;
import :strings_to_automaton;
import :types;

import std;

namespace lucene {

Result<Automaton> Automata::make_empty() {
    Automaton a;
    if (auto finished = a.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }
    return a;
}

Result<Automaton> Automata::make_empty_string() {
    Automaton a;
    (void)a.create_state();
    a.set_accept(0, true);
    return a;
}

Result<Automaton> Automata::make_any_string() {
    Automaton a;
    const int32_t s = a.create_state();
    a.set_accept(s, true);
    if (auto added = a.add_transition(s, s, 0, MAX_CODE_POINT); !added) {
        return err(std::move(added.error()));
    }
    if (auto finished = a.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }
    return a;
}

Result<Automaton> Automata::make_any_binary() {
    Automaton a;
    const int32_t s = a.create_state();
    a.set_accept(s, true);
    if (auto added = a.add_transition(s, s, 0, 255); !added) {
        return err(std::move(added.error()));
    }
    if (auto finished = a.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }
    return a;
}

Result<Automaton> Automata::make_non_empty_binary() {
    Automaton a;
    const int32_t s1 = a.create_state();
    const int32_t s2 = a.create_state();
    a.set_accept(s2, true);
    if (auto added = a.add_transition(s1, s2, 0, 255); !added) {
        return err(std::move(added.error()));
    }
    if (auto added = a.add_transition(s2, s2, 0, 255); !added) {
        return err(std::move(added.error()));
    }
    if (auto finished = a.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }
    return a;
}

Result<Automaton> Automata::make_any_char() { return make_char_range(0, MAX_CODE_POINT); }

Result<int32_t> Automata::append_any_char(Automaton &a, int32_t state) {
    const int32_t new_state = a.create_state();
    if (auto added = a.add_transition(state, new_state, 0, MAX_CODE_POINT); !added) {
        return err(std::move(added.error()));
    }
    return new_state;
}

Result<Automaton> Automata::make_char(int32_t c) { return make_char_range(c, c); }

Result<int32_t> Automata::append_char(Automaton &a, int32_t state, int32_t c) {
    const int32_t new_state = a.create_state();
    if (auto added = a.add_transition(state, new_state, c, c); !added) {
        return err(std::move(added.error()));
    }
    return new_state;
}

Result<Automaton> Automata::make_char_range(int32_t min, int32_t max) {
    if (min > max) {
        return make_empty();
    }
    Automaton a;
    const int32_t s1 = a.create_state();
    const int32_t s2 = a.create_state();
    a.set_accept(s2, true);
    if (auto added = a.add_transition(s1, s2, min, max); !added) {
        return err(std::move(added.error()));
    }
    if (auto finished = a.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }
    return a;
}

Result<int32_t> Automata::any_of_right_length(AutomatonBuilder &builder, std::string_view x, size_t n) {
    const int32_t s = builder.create_state();
    if (x.size() == n) {
        builder.set_accept(s, true);
    } else {
        auto next = any_of_right_length(builder, x, n + 1);
        if (!next) {
            return err(std::move(next.error()));
        }
        if (auto added = builder.add_transition(s, next.value(), '0', '9'); !added) {
            return err(std::move(added.error()));
        }
    }
    return s;
}

Result<int32_t> Automata::at_least(AutomatonBuilder &builder, std::string_view x, size_t n,
                                   std::vector<int32_t> &initials, bool zeros) {
    const int32_t s = builder.create_state();
    if (x.size() == n) {
        builder.set_accept(s, true);
    } else {
        if (zeros) {
            initials.push_back(s);
        }
        const auto c = static_cast<int32_t>(static_cast<uint8_t>(x[n]));
        auto value = at_least(builder, x, n + 1, initials, zeros && c == '0');
        if (!value) {
            return err(std::move(value.error()));
        }
        if (auto added = builder.add_transition_label(s, value.value(), c); !added) {
            return err(std::move(added.error()));
        }
        if (c < '9') {
            auto rest = any_of_right_length(builder, x, n + 1);
            if (!rest) {
                return err(std::move(rest.error()));
            }
            if (auto added = builder.add_transition(s, rest.value(), c + 1, '9'); !added) {
                return err(std::move(added.error()));
            }
        }
    }
    return s;
}

Result<int32_t> Automata::at_most(AutomatonBuilder &builder, std::string_view x, size_t n) {
    const int32_t s = builder.create_state();
    if (x.size() == n) {
        builder.set_accept(s, true);
    } else {
        const auto c = static_cast<int32_t>(static_cast<uint8_t>(x[n]));
        auto value = at_most(builder, x, n + 1);
        if (!value) {
            return err(std::move(value.error()));
        }
        if (auto added = builder.add_transition_label(s, value.value(), c); !added) {
            return err(std::move(added.error()));
        }
        if (c > '0') {
            auto rest = any_of_right_length(builder, x, n + 1);
            if (!rest) {
                return err(std::move(rest.error()));
            }
            if (auto added = builder.add_transition(s, rest.value(), '0', c - 1); !added) {
                return err(std::move(added.error()));
            }
        }
    }
    return s;
}

Result<int32_t> Automata::between(AutomatonBuilder &builder, std::string_view x, std::string_view y, size_t n,
                                  std::vector<int32_t> &initials, bool zeros) {
    const int32_t s = builder.create_state();
    if (x.size() == n) {
        builder.set_accept(s, true);
    } else {
        if (zeros) {
            initials.push_back(s);
        }
        const auto cx = static_cast<int32_t>(static_cast<uint8_t>(x[n]));
        const auto cy = static_cast<int32_t>(static_cast<uint8_t>(y[n]));

        if (cx == cy) {
            auto value = between(builder, x, y, n + 1, initials, zeros && cx == '0');
            if (!value) {
                return err(std::move(value.error()));
            }
            if (auto added = builder.add_transition(s, value.value(), cx, cx); !added) {
                return err(std::move(added.error()));
            }
        } else {
            auto least = at_least(builder, x, n + 1, initials, zeros && cx == '0');
            if (!least) {
                return err(std::move(least.error()));
            }
            if (auto added = builder.add_transition(s, least.value(), cx, cx); !added) {
                return err(std::move(added.error()));
            }
            auto most = at_most(builder, y, n + 1);
            if (!most) {
                return err(std::move(most.error()));
            }
            if (auto added = builder.add_transition(s, most.value(), cy, cy); !added) {
                return err(std::move(added.error()));
            }
            if (cx + 1 < cy) {
                auto rest = any_of_right_length(builder, x, n + 1);
                if (!rest) {
                    return err(std::move(rest.error()));
                }
                if (auto added = builder.add_transition(s, rest.value(), cx + 1, cy - 1); !added) {
                    return err(std::move(added.error()));
                }
            }
        }
    }
    return s;
}

bool Automata::suffix_is_zeros(const BytesRef &br, size_t from) {
    for (size_t i = from; i < br.length(); ++i) {
        if (br.bytes()[br.offset() + i] != 0) {
            return false;
        }
    }
    return true;
}

Result<Automaton> Automata::make_binary_interval(const BytesRef *min, bool min_inclusive, const BytesRef *max,
                                                 bool max_inclusive) {
    if (min == nullptr && !min_inclusive) {
        return err(LuceneError::illegal_argument("minInclusive must be true when min is null"));
    }
    if (max == nullptr && !max_inclusive) {
        return err(LuceneError::illegal_argument("maxInclusive must be true when max is null"));
    }

    const BytesRef empty_min;
    bool min_incl = min_inclusive;
    const BytesRef *lower = min;
    if (lower == nullptr) {
        min_incl = true;
        lower = &empty_min;
    }

    int32_t cmp = 0;
    if (max != nullptr) {
        cmp = lower->compare_to(*max);
    } else {
        if (lower->length() == 0) {
            return min_incl ? make_any_binary() : make_non_empty_binary();
        }
        cmp = -1;
    }

    if (cmp == 0) {
        return (!min_incl || !max_inclusive) ? make_empty() : make_binary(*lower);
    }
    if (cmp > 0) {
        return make_empty();
    }

    if (max != nullptr && StringHelper::starts_with(max->as_slice(), lower->as_slice()) &&
        suffix_is_zeros(*max, lower->length())) {
        size_t max_length = max->length();

        debug_check(max_length > lower->length(), "the maximum is longer than the minimum");
        if (!max_inclusive) {
            max_length -= 1;
        }

        if (max_length == lower->length()) {
            return !min_incl ? make_empty() : make_binary(*lower);
        }

        Automaton a;
        int32_t last_state = a.create_state();
        for (size_t i = 0; i < lower->length(); ++i) {
            const int32_t state = a.create_state();
            const auto label = static_cast<int32_t>(lower->bytes()[lower->offset() + i]);
            if (auto added = a.add_transition_label(last_state, state, label); !added) {
                return err(std::move(added.error()));
            }
            last_state = state;
        }

        if (min_incl) {
            a.set_accept(last_state, true);
        }

        for (size_t i = lower->length(); i < max_length; ++i) {
            const int32_t state = a.create_state();
            if (auto added = a.add_transition_label(last_state, state, 0); !added) {
                return err(std::move(added.error()));
            }
            a.set_accept(state, true);
            last_state = state;
        }

        if (auto finished = a.finish_state(); !finished) {
            return err(std::move(finished.error()));
        }
        return a;
    }

    // General case.
    Automaton a;
    const int32_t start_state = a.create_state();
    const int32_t sink_state = a.create_state();
    a.set_accept(sink_state, true);
    if (auto added = a.add_transition(sink_state, sink_state, 0, 255); !added) {
        return err(std::move(added.error()));
    }

    bool equal_prefix = true;
    int32_t last_state = start_state;
    int32_t first_max_state = -1;
    size_t shared_prefix_length = 0;

    for (size_t i = 0; i < lower->length(); ++i) {
        const auto min_label = static_cast<int32_t>(lower->bytes()[lower->offset() + i]);

        int32_t max_label = -1;
        if (max != nullptr && equal_prefix && i < max->length()) {
            max_label = static_cast<int32_t>(max->bytes()[max->offset() + i]);
        }

        int32_t next_state = -1;
        if (min_incl && i == lower->length() - 1 && (!equal_prefix || min_label != max_label)) {
            next_state = sink_state;
        } else {
            next_state = a.create_state();
        }

        if (equal_prefix) {
            if (min_label == max_label) {
                if (auto added = a.add_transition_label(last_state, next_state, min_label); !added) {
                    return err(std::move(added.error()));
                }
            } else if (max != nullptr) {
                debug_check(max_label > min_label, "the maximum label is greater");
                if (auto added = a.add_transition_label(last_state, next_state, min_label); !added) {
                    return err(std::move(added.error()));
                }
                if (max_label > min_label + 1) {
                    if (auto added = a.add_transition(last_state, sink_state, min_label + 1, max_label - 1); !added) {
                        return err(std::move(added.error()));
                    }
                }

                if (max_inclusive || i < max->length() - 1) {
                    first_max_state = a.create_state();
                    if (i < max->length() - 1) {
                        a.set_accept(first_max_state, true);
                    }
                    if (auto added = a.add_transition_label(last_state, first_max_state, max_label); !added) {
                        return err(std::move(added.error()));
                    }
                }
                equal_prefix = false;
                shared_prefix_length = i;
            } else {
                equal_prefix = false;
                shared_prefix_length = 0;
                if (auto added = a.add_transition(last_state, sink_state, min_label + 1, 255); !added) {
                    return err(std::move(added.error()));
                }
                if (auto added = a.add_transition_label(last_state, next_state, min_label); !added) {
                    return err(std::move(added.error()));
                }
            }
        } else {
            if (auto added = a.add_transition_label(last_state, next_state, min_label); !added) {
                return err(std::move(added.error()));
            }
            if (min_label < 255) {
                if (auto added = a.add_transition(last_state, sink_state, min_label + 1, 255); !added) {
                    return err(std::move(added.error()));
                }
            }
        }

        last_state = next_state;
    }

    if (!equal_prefix && last_state != sink_state && last_state != start_state) {
        if (auto added = a.add_transition(last_state, sink_state, 0, 255); !added) {
            return err(std::move(added.error()));
        }
    }

    if (min_incl) {
        a.set_accept(last_state, true);
    }

    if (max != nullptr) {
        if (first_max_state == -1) {
            shared_prefix_length = lower->length();
        } else {
            last_state = first_max_state;
            shared_prefix_length += 1;
        }

        for (size_t i = shared_prefix_length; i < max->length(); ++i) {
            const auto max_label = static_cast<int32_t>(max->bytes()[max->offset() + i]);
            if (max_label > 0) {
                if (auto added = a.add_transition(last_state, sink_state, 0, max_label - 1); !added) {
                    return err(std::move(added.error()));
                }
            }
            if (max_inclusive || i < max->length() - 1) {
                const int32_t next_state = a.create_state();
                if (i < max->length() - 1) {
                    a.set_accept(next_state, true);
                }
                if (auto added = a.add_transition_label(last_state, next_state, max_label); !added) {
                    return err(std::move(added.error()));
                }
                last_state = next_state;
            }
        }

        if (max_inclusive) {
            a.set_accept(last_state, true);
        }
    }

    if (auto finished = a.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }

    debug_check(a.is_deterministic(), "the interval automaton is deterministic");

    return a;
}

Result<Automaton> Automata::make_decimal_interval(int32_t min, int32_t max, int32_t digits) {
    std::string x = std::to_string(min);
    std::string y = std::to_string(max);

    if (min > max || (digits > 0 && y.size() > static_cast<size_t>(digits))) {
        return err(LuceneError::illegal_argument("invalid min/max/digits for make_decimal_interval"));
    }

    const size_t d = digits > 0 ? static_cast<size_t>(digits) : y.size();
    if (x.size() < d) {
        x = std::string(d - x.size(), '0') + x;
    }
    if (y.size() < d) {
        y = std::string(d - y.size(), '0') + y;
    }

    AutomatonBuilder builder;

    if (digits <= 0) {
        (void)builder.create_state();
    }

    std::vector<int32_t> initials;
    if (digits <= 0) {
        initials.reserve(y.size());
    }
    if (auto added = between(builder, x, y, 0, initials, digits <= 0); !added) {
        return err(std::move(added.error()));
    }

    auto a1 = builder.finish();
    if (!a1) {
        return err(std::move(a1.error()));
    }

    if (digits <= 0) {
        if (auto added = a1.value().add_transition_label(0, 0, '0'); !added) {
            return err(std::move(added.error()));
        }
        for (const int32_t p : initials) {
            if (auto added = a1.value().add_epsilon(0, p); !added) {
                return err(std::move(added.error()));
            }
        }
        if (auto finished = a1.value().finish_state(); !finished) {
            return err(std::move(finished.error()));
        }
    }

    return a1;
}

Result<Automaton> Automata::make_string(std::string_view s) {
    Automaton a;
    int32_t last_state = a.create_state();

    auto code_points = StringHelper::utf8_to_code_points(s);
    if (!code_points) {
        return err(std::move(code_points.error()));
    }
    for (const int32_t code_point : code_points.value()) {
        const int32_t state = a.create_state();
        if (auto added = a.add_transition_label(last_state, state, code_point); !added) {
            return err(std::move(added.error()));
        }
        last_state = state;
    }

    a.set_accept(last_state, true);
    if (auto finished = a.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }

    debug_check(a.is_deterministic(), "the string automaton is deterministic");
    auto dead = Operations::has_dead_states(a);
    debug_check(dead.has_value() && !dead.value(), "the string automaton has no dead states");
    return a;
}

Result<Automaton> Automata::make_binary(const BytesRef &term) {
    Automaton a;
    int32_t last_state = a.create_state();

    for (size_t i = 0; i < term.length(); ++i) {
        const int32_t state = a.create_state();
        const auto label = static_cast<int32_t>(term.bytes()[term.offset() + i]);
        if (auto added = a.add_transition_label(last_state, state, label); !added) {
            return err(std::move(added.error()));
        }
        last_state = state;
    }

    a.set_accept(last_state, true);
    if (auto finished = a.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }

    debug_check(a.is_deterministic(), "the binary automaton is deterministic");
    auto dead = Operations::has_dead_states(a);
    debug_check(dead.has_value() && !dead.value(), "the binary automaton has no dead states");
    return a;
}

Result<Automaton> Automata::make_string_from_codepoints(std::span<const int32_t> word, size_t offset, size_t length) {
    Automaton a;
    (void)a.create_state();
    int32_t s = 0;
    for (size_t i = offset; i < offset + length; ++i) {
        const int32_t s2 = a.create_state();
        if (auto added = a.add_transition_label(s, s2, word[i]); !added) {
            return err(std::move(added.error()));
        }
        s = s2;
    }
    a.set_accept(s, true);
    if (auto finished = a.finish_state(); !finished) {
        return err(std::move(finished.error()));
    }
    return a;
}

Result<Automaton> Automata::make_string_union(std::span<const BytesRef> utf8_strings) {
    if (utf8_strings.empty()) {
        return make_empty();
    }
    return StringsToAutomaton::build(utf8_strings, false);
}

Result<Automaton> Automata::make_binary_string_union(std::span<const BytesRef> utf8_strings) {
    if (utf8_strings.empty()) {
        return make_empty();
    }
    return StringsToAutomaton::build(utf8_strings, true);
}

Result<Automaton> Automata::make_string_union_from_iter(BytesRefIterator &utf8_strings) {
    return StringsToAutomaton::build_from_iterator(utf8_strings, false);
}

Result<Automaton> Automata::make_binary_string_union_from_iter(BytesRefIterator &utf8_strings) {
    return StringsToAutomaton::build_from_iterator(utf8_strings, true);
}

} // namespace lucene
