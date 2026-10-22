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

module lucene_core:strings_to_automaton.impl;

import :strings_to_automaton;

import :automaton;
import :bytes_ref;
import :bytes_ref_builder;
import :bytes_ref_iterator;
import :debug;
import :error;
import :string_helper;
import :types;

import std;

namespace lucene {

StringsToAutomaton::StringsToAutomaton() : all_states_{State{}} {}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

std::optional<size_t> StringsToAutomaton::State::get_state(int32_t label) const {
    const auto found = std::lower_bound(labels.begin(), labels.end(), label);
    if (found == labels.end() || *found != label) {
        return std::nullopt;
    }
    return states[static_cast<size_t>(found - labels.begin())];
}

size_t StringsToAutomaton::State::new_state(int32_t label, size_t new_state_index) {
    debug_check(!std::binary_search(labels.begin(), labels.end(), label), "the label is already used");
    labels.push_back(label);
    states.push_back(new_state_index);
    return new_state_index;
}

Result<size_t> StringsToAutomaton::State::last_child() const {
    if (states.empty()) {
        return err(LuceneError::illegal_state("state has no outgoing transitions"));
    }
    return states.back();
}

std::optional<size_t> StringsToAutomaton::State::last_child_with_label(int32_t label) const {
    const size_t index = labels.size();
    std::optional<size_t> state;
    if (index > 0 && labels[index - 1] == label) {
        state = states[index - 1];
    }
    debug_check(state == get_state(label), "the last child is the one the lookup finds");
    return state;
}

void StringsToAutomaton::State::replace_last_child(size_t state) {
    debug_check(has_children(), "the state has outgoing transitions");
    states.back() = state;
}

bool StringsToAutomaton::State::operator<(const State &other) const {
    if (is_final != other.is_final) {
        return is_final < other.is_final;
    }
    if (labels != other.labels) {
        return labels < other.labels;
    }
    return states < other.states;
}

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

VoidResult StringsToAutomaton::set_previous(const BytesRef &current) {
    if (!previous_.has_value()) {
        previous_.emplace();
    }
    return previous_->copy_bytes(current);
}

size_t StringsToAutomaton::create_state() {
    all_states_.emplace_back();
    return all_states_.size() - 1;
}

Result<int32_t> StringsToAutomaton::convert(AutomatonBuilder &a, size_t state, const std::vector<State> &all_states,
                                            std::unordered_map<size_t, int32_t> &visited) {
    int32_t converted = -1;
    const auto found = visited.find(state);
    if (found != visited.end()) {
        return found->second;
    }
    converted = a.create_state();
    const State &s = all_states[state];
    a.set_accept(converted, s.is_final);
    visited.emplace(state, converted);

    for (size_t i = 0; i < s.states.size(); ++i) {
        auto target = convert(a, s.states[i], all_states, visited);
        if (!target) {
            return err(std::move(target.error()));
        }
        if (auto added = a.add_transition_label(converted, target.value(), s.labels[i]); !added) {
            return err(std::move(added.error()));
        }
    }

    return converted;
}

Result<Automaton> StringsToAutomaton::complete_and_convert() {
    if (all_states_[root_].has_children()) {
        if (auto registered = replace_or_register(root_); !registered) {
            return err(std::move(registered.error()));
        }
    }

    state_registry_.clear();

    AutomatonBuilder builder;
    std::unordered_map<size_t, int32_t> visited;
    visited.reserve(all_states_.size());
    auto converted = convert(builder, root_, all_states_, visited);
    if (!converted) {
        return err(std::move(converted.error()));
    }
    return builder.finish();
}

Result<Automaton> StringsToAutomaton::build(std::span<const BytesRef> input, bool as_binary) {
    StringsToAutomaton builder;

    for (const BytesRef &term : input) {
        if (auto added = builder.add(term, as_binary); !added) {
            return err(std::move(added.error()));
        }
    }

    return builder.complete_and_convert();
}

Result<Automaton> StringsToAutomaton::build_from_iterator(BytesRefIterator &input, bool as_binary) {
    StringsToAutomaton builder;

    while (true) {
        auto next = input.next();
        if (!next) {
            return err(std::move(next.error()));
        }
        if (next.value() == nullptr) {
            break;
        }
        if (auto added = builder.add(*next.value(), as_binary); !added) {
            return err(std::move(added.error()));
        }
    }

    return builder.complete_and_convert();
}

VoidResult StringsToAutomaton::add(const BytesRef &current, bool as_binary) {
    if (current.length() > static_cast<size_t>(MAX_STRING_UNION_TERM_LENGTH)) {
        return err(LuceneError::illegal_argument(std::format(
                "This builder doesn't allow terms that are larger than {} UTF-8 bytes, got {}",
                MAX_STRING_UNION_TERM_LENGTH, current.to_display_string())));
    }

    if (previous_.has_value() && previous_->bytes().compare_to(current) > 0) {
        return err(LuceneError::illegal_argument(
                std::format("Input must be in sorted UTF-8 order: {} >= {}",
                            previous_->bytes().to_display_string(), current.to_display_string())));
    }
    if (auto set = set_previous(current); !set) {
        return set;
    }

    const std::vector<uint8_t> &bytes = current.bytes();
    const std::span<const uint8_t> all_bytes(bytes);
    size_t pos = current.offset();
    const size_t end = current.offset() + current.length();
    size_t state = root_;

    if (as_binary) {
        while (pos < end) {
            const auto label = static_cast<int32_t>(bytes[pos]);
            const std::optional<size_t> child = all_states_[state].last_child_with_label(label);
            if (!child.has_value()) {
                break;
            }
            state = child.value();
            pos += 1;
        }
    } else {
        while (pos < end) {
            size_t num_bytes = 0;
            auto code_point = StringHelper::utf8_code_point_at(all_bytes, pos, num_bytes);
            if (!code_point) {
                return err(std::move(code_point.error()));
            }
            const std::optional<size_t> child = all_states_[state].last_child_with_label(code_point.value());
            if (!child.has_value()) {
                break;
            }
            state = child.value();
            pos += num_bytes;
        }
    }

    if (all_states_[state].has_children()) {
        if (auto registered = replace_or_register(state); !registered) {
            return registered;
        }
    }

    if (as_binary) {
        while (pos < end) {
            state = new_state(state, static_cast<int32_t>(bytes[pos]));
            pos += 1;
        }
    } else {
        while (pos < end) {
            size_t num_bytes = 0;
            auto code_point = StringHelper::utf8_code_point_at(all_bytes, pos, num_bytes);
            if (!code_point) {
                return err(std::move(code_point.error()));
            }
            state = new_state(state, code_point.value());
            pos += num_bytes;
        }
    }

    all_states_[state].is_final = true;

    return {};
}

size_t StringsToAutomaton::new_state(size_t state, int32_t label) {
    const size_t created = create_state();
    return all_states_[state].new_state(label, created);
}

VoidResult StringsToAutomaton::replace_or_register(size_t state) {
    auto child = all_states_[state].last_child();
    if (!child) {
        return err(std::move(child.error()));
    }

    if (all_states_[child.value()].has_children()) {
        if (auto registered = replace_or_register(child.value()); !registered) {
            return registered;
        }
    }

    const auto found = state_registry_.find(all_states_[child.value()]);
    if (found != state_registry_.end()) {
        all_states_[state].replace_last_child(found->second);
    } else {
        state_registry_.emplace(all_states_[child.value()], child.value());
    }
    return {};
}

} // namespace lucene
