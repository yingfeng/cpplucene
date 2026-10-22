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

module lucene_core:automaton_terms_enum.impl;

import :automaton_terms_enum;

import :bytes_ref;
import :bytes_ref_builder;
import :compiled_automaton;
import :error;
import :filtered_terms_enum;
import :ints_ref;
import :string_helper;
import :terms_enum;
import :transition;
import :types;

import std;

namespace lucene {

AutomatonTermsEnum::AutomatonTermsEnum(AutomatonEnum automaton, std::optional<BytesRef> common_suffix, bool finite,
                                       std::optional<BytesRef> start_term)
    : automaton_(std::move(automaton)), common_suffix_(std::move(common_suffix)), finite_(finite),
      start_term_(std::move(start_term)) {}

Result<std::unique_ptr<FilteredTermsEnum>> AutomatonTermsEnum::create(std::unique_ptr<TermsEnum> tenum,
                                                                     const CompiledAutomaton &compiled,
                                                                     const BytesRef *start_term) {
    if (compiled.type() != AutomatonType::kNormal) {
        return err(LuceneError::illegal_argument("please use CompiledAutomaton.get_terms_enum instead"));
    }

    auto byte_runnable = compiled.get_automaton();
    if (!byte_runnable) {
        return std::unexpected(std::move(byte_runnable.error()));
    }

    std::optional<BytesRef> start;
    if (start_term != nullptr) {
        start = *start_term;
    }

    auto hook = std::unique_ptr<AutomatonTermsEnum>(new AutomatonTermsEnum(
            std::move(byte_runnable.value()), compiled.common_suffix_ref(), compiled.finite(), std::move(start)));
    if (!compiled.finite()) {
        hook->visited_.assign(hook->automaton_.get_size(), 0);
    }
    return FilteredTermsEnum::create(std::move(tenum), std::move(hook), true);
}

VoidResult AutomatonTermsEnum::set_visited(int32_t state) {
    const size_t index = static_cast<size_t>(state);
    if (!finite_) {
        if (index >= visited_.size()) {
            visited_.resize(index + 1, 0);
        }
        visited_[index] = cur_gen_;
    }
    return {};
}

bool AutomatonTermsEnum::is_visited(int32_t state) const {
    const size_t index = static_cast<size_t>(state);
    return !finite_ && index < visited_.size() && visited_[index] == cur_gen_;
}

VoidResult AutomatonTermsEnum::set_linear(size_t position) {
    int32_t state = 0;
    int32_t max_interval = 0xff;

    for (size_t i = 0; i < position; ++i) {
        const int32_t byte = static_cast<int32_t>(seek_bytes_ref_.byte_at(i));
        auto next = automaton_.step(state, byte);
        if (!next) {
            return std::unexpected(std::move(next.error()));
        }
        state = next.value();
    }

    auto num_transitions = automaton_.get_num_transitions_with_state(state);
    if (!num_transitions) {
        return std::unexpected(std::move(num_transitions.error()));
    }
    auto init = automaton_.init_transition(state, transition_);
    if (!init) {
        return std::unexpected(std::move(init.error()));
    }

    for (int32_t i = 0; i < num_transitions.value(); ++i) {
        auto next_transition = automaton_.get_next_transition(transition_);
        if (!next_transition) {
            return std::unexpected(std::move(next_transition.error()));
        }
        const int32_t ch = static_cast<int32_t>(seek_bytes_ref_.byte_at(position));
        if (transition_.min <= ch && ch <= transition_.max) {
            max_interval = transition_.max;
            break;
        }
    }
    // 0xff terms don't get the optimization... not worth the trouble.
    if (max_interval < 0xff) {
        ++max_interval;
    }

    const size_t length = position + 1;
    std::vector<uint8_t> bytes(length, 0);
    for (size_t i = 0; i < position; ++i) {
        bytes[i] = seek_bytes_ref_.byte_at(i);
    }
    bytes[position] = static_cast<uint8_t>(max_interval);
    linear_upper_bound_ = BytesRef::from_slice(std::move(bytes), 0, length);

    linear_ = true;
    return {};
}

Result<bool> AutomatonTermsEnum::next_string() {
    int32_t state = 0;
    size_t pos = 0;

    auto grown = saved_states_.grow(seek_bytes_ref_.length() + 1);
    if (!grown) {
        return std::unexpected(std::move(grown.error()));
    }
    saved_states_.set_int_at(0, 0);

    while (true) {
        if (!finite_) {
            ++cur_gen_;
            if (cur_gen_ == 0) {
                // Clear the visited states every time the generation wraps (so
                // very infrequently to not impact average performance).
                std::ranges::fill(visited_, std::numeric_limits<uint16_t>::max());
            }
        }

        linear_ = false;

        // walk the automaton until a character is rejected
        state = saved_states_.int_at(pos);
        while (pos < seek_bytes_ref_.length()) {
            auto visited = set_visited(state);
            if (!visited) {
                return std::unexpected(std::move(visited.error()));
            }
            const int32_t byte = static_cast<int32_t>(seek_bytes_ref_.byte_at(pos));
            auto next = automaton_.step(state, byte);
            if (!next) {
                return std::unexpected(std::move(next.error()));
            }
            const int32_t next_state = next.value();
            if (next_state == -1) {
                break;
            }
            saved_states_.set_int_at(pos + 1, next_state);
            if (!linear_ && is_visited(next_state)) {
                auto linear = set_linear(pos);
                if (!linear) {
                    return std::unexpected(std::move(linear.error()));
                }
            }
            state = next_state;
            ++pos;
        }

        // take the useful portion, and the last non-reject state, and attempt to
        // append characters that will match.
        auto matched = next_string_with_position(state, pos);
        if (!matched) {
            return std::unexpected(std::move(matched.error()));
        }
        if (matched.value()) {
            return true;
        }

        // no more solutions exist from this useful portion, backtrack
        auto previous_position = backtrack(pos);
        if (!previous_position.has_value()) {
            // no more solutions at all
            return false;
        }
        pos = previous_position.value();

        const int32_t prev_state = saved_states_.int_at(pos);
        const int32_t byte = static_cast<int32_t>(seek_bytes_ref_.byte_at(pos));
        auto new_state = automaton_.step(prev_state, byte);
        if (!new_state) {
            return std::unexpected(std::move(new_state.error()));
        }
        if (new_state.value() >= 0) {
            auto accepted = automaton_.is_accept(new_state.value());
            if (!accepted) {
                return std::unexpected(std::move(accepted.error()));
            }
            if (accepted.value()) {
                // String is good to go as-is
                return true;
            }
        }

        if (!finite_) {
            pos = 0;
        }
    }
}

Result<bool> AutomatonTermsEnum::next_string_with_position(int32_t state, size_t position) {
    // The next lexicographic character must be greater than the existing one.
    int32_t c = 0;
    if (position < seek_bytes_ref_.length()) {
        c = static_cast<int32_t>(seek_bytes_ref_.byte_at(position));
        // If the next byte is 0xff and is not part of the useful portion, then
        // by definition it puts us in a reject state, so this path is dead:
        // there cannot be any higher transitions.  Backtrack.
        if (c == 0xff) {
            return false;
        }
        ++c;
    }

    seek_bytes_ref_.set_length(position);
    auto visited = set_visited(state);
    if (!visited) {
        return std::unexpected(std::move(visited.error()));
    }

    auto num_transitions = automaton_.get_num_transitions_with_state(state);
    if (!num_transitions) {
        return std::unexpected(std::move(num_transitions.error()));
    }
    auto init = automaton_.init_transition(state, transition_);
    if (!init) {
        return std::unexpected(std::move(init.error()));
    }

    // find the minimal path (lexicographic order) that is >= c
    for (int32_t i = 0; i < num_transitions.value(); ++i) {
        auto next_transition = automaton_.get_next_transition(transition_);
        if (!next_transition) {
            return std::unexpected(std::move(next_transition.error()));
        }
        if (transition_.max < c) {
            continue;
        }

        const int32_t next_char = std::max(transition_.min, c);
        // append either the next sequential char, or the minimum transition
        auto appended = seek_bytes_ref_.append_byte(static_cast<uint8_t>(next_char));
        if (!appended) {
            return std::unexpected(std::move(appended.error()));
        }
        state = transition_.dest;

        // as long as is possible, continue down the minimal path in
        // lexicographic order; stop when a loop or an accept state is found.
        while (true) {
            if (is_visited(state)) {
                break;
            }
            auto accepted = automaton_.is_accept(state);
            if (!accepted) {
                return std::unexpected(std::move(accepted.error()));
            }
            if (accepted.value()) {
                break;
            }
            auto visit = set_visited(state);
            if (!visit) {
                return std::unexpected(std::move(visit.error()));
            }
            // Note: we work with a DFA with no transitions to dead states, so
            // if this is not an accept state then there MUST be at least one
            // transition.
            auto inner_init = automaton_.init_transition(state, transition_);
            if (!inner_init) {
                return std::unexpected(std::move(inner_init.error()));
            }
            auto inner_next = automaton_.get_next_transition(transition_);
            if (!inner_next) {
                return std::unexpected(std::move(inner_next.error()));
            }
            state = transition_.dest;
            // append the minimum transition
            auto inner_appended = seek_bytes_ref_.append_byte(static_cast<uint8_t>(transition_.min));
            if (!inner_appended) {
                return std::unexpected(std::move(inner_appended.error()));
            }
            // we found a loop, record it for faster enumeration
            if (!linear_ && is_visited(state)) {
                auto linear = set_linear(seek_bytes_ref_.length() - 1);
                if (!linear) {
                    return std::unexpected(std::move(linear.error()));
                }
            }
        }
        return true;
    }

    return false;
}

std::optional<size_t> AutomatonTermsEnum::backtrack(size_t position) {
    while (position > 0) {
        --position;
        const uint8_t next_char = seek_bytes_ref_.byte_at(position);
        // if a character is 0xff it's a dead-end too, because there is no
        // higher character in binary sort order.
        if (next_char != 0xff) {
            seek_bytes_ref_.set_byte_at(position, static_cast<uint8_t>(next_char + 1));
            seek_bytes_ref_.set_length(position + 1);
            return position;
        }
    }
    return std::nullopt;
}

Result<AcceptStatus> AutomatonTermsEnum::accept(const BytesRef &term, int64_t /*ord*/) {
    bool suffix_ok = true;
    if (common_suffix_.has_value()) {
        suffix_ok = StringHelper::ends_with(term.as_slice(), common_suffix_->as_slice());
    }

    const bool below_upper_bound = linear_ && term.compare_to(linear_upper_bound_) < 0;

    if (suffix_ok) {
        auto run = automaton_.run(term.as_slice(), 0, term.length());
        if (!run) {
            return std::unexpected(std::move(run.error()));
        }
        if (run.value()) {
            return linear_ ? AcceptStatus::kYes : AcceptStatus::kYesAndSeek;
        }
        return below_upper_bound ? AcceptStatus::kNo : AcceptStatus::kNoAndSeek;
    }
    return below_upper_bound ? AcceptStatus::kNo : AcceptStatus::kNoAndSeek;
}

Result<std::optional<BytesRef>> AutomatonTermsEnum::next_seek_term(const BytesRef *term) {
    if (term != nullptr) {
        auto copied = seek_bytes_ref_.copy_bytes(*term);
        if (!copied) {
            return std::unexpected(std::move(copied.error()));
        }
    } else if (start_term_.has_value()) {
        auto copied = seek_bytes_ref_.copy_bytes(start_term_.value());
        if (!copied) {
            return std::unexpected(std::move(copied.error()));
        }
    } else if (seek_bytes_ref_.length() == 0) {
        // return the empty term, as it's valid
        auto accepted = automaton_.is_accept(0);
        if (!accepted) {
            return std::unexpected(std::move(accepted.error()));
        }
        if (accepted.value()) {
            return std::optional<BytesRef>(seek_bytes_ref_.get_bytes_ref());
        }
    }

    auto more = next_string();
    if (!more) {
        return std::unexpected(std::move(more.error()));
    }
    if (!more.value()) {
        return std::optional<BytesRef>{};
    }
    return std::optional<BytesRef>(seek_bytes_ref_.get_bytes_ref());
}

} // namespace lucene
