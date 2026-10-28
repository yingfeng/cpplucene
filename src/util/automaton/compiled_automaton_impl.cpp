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

module lucene_core:compiled_automaton.impl;

import :compiled_automaton;

import :automata;
import :automaton;
import :byte_run_automaton;
import :byte_runnable;
import :bytes_ref;
import :bytes_ref_builder;
import :error;
import :ints_ref;
import :nfa_run_automaton;
import :operations;
import :query_visitor;
import :run_automaton;
import :string_helper;
import :transition;
import :term;
import :transition_accessor;
import :types;
import :utf32_to_utf8;

import std;

namespace lucene {

// ===========================================================================
// AutomatonEnum
// ===========================================================================

Result<int32_t> AutomatonEnum::step(int32_t state, int32_t c) {
    if (run_) {
        return run_->step(state, c);
    }
    if (nfa_) {
        return nfa_->step(state, c);
    }
    return std::unexpected(LuceneError::illegal_state("AutomatonEnum has no automaton"));
}

Result<bool> AutomatonEnum::is_accept(int32_t state) const {
    if (run_) {
        return run_->is_accept(state);
    }
    if (nfa_) {
        return nfa_->is_accept(state);
    }
    return std::unexpected(LuceneError::illegal_state("AutomatonEnum has no automaton"));
}

size_t AutomatonEnum::get_size() const {
    if (run_) {
        return run_->get_size();
    }
    if (nfa_) {
        return nfa_->get_size();
    }
    return 0;
}

Result<bool> AutomatonEnum::run(std::span<const uint8_t> s, size_t offset, size_t length) {
    if (run_) {
        return run_->run(s, offset, length);
    }
    if (nfa_) {
        return nfa_->run(s, offset, length);
    }
    return std::unexpected(LuceneError::illegal_state("AutomatonEnum has no automaton"));
}

Result<int32_t> AutomatonEnum::init_transition(int32_t state, Transition &t) {
    if (run_) {
        return run_->base().automaton().init_transition(state, t);
    }
    if (nfa_) {
        return nfa_->init_transition(state, t);
    }
    return std::unexpected(LuceneError::illegal_state("AutomatonEnum has no automaton"));
}

VoidResult AutomatonEnum::get_next_transition(Transition &t) {
    if (run_) {
        return run_->base().automaton().get_next_transition(t);
    }
    if (nfa_) {
        return nfa_->get_next_transition(t);
    }
    return std::unexpected(LuceneError::illegal_state("AutomatonEnum has no automaton"));
}

Result<int32_t> AutomatonEnum::get_num_transitions_with_state(int32_t state) {
    if (run_) {
        return run_->base().automaton().get_num_transitions_with_state(state);
    }
    if (nfa_) {
        return nfa_->get_num_transitions_with_state(state);
    }
    return std::unexpected(LuceneError::illegal_state("AutomatonEnum has no automaton"));
}

VoidResult AutomatonEnum::get_transition(int32_t state, int32_t index, Transition &t) {
    if (run_) {
        run_->base().automaton().get_transition(state, index, t);
        return {};
    }
    if (nfa_) {
        return nfa_->get_transition(state, index, t);
    }
    return std::unexpected(LuceneError::illegal_state("AutomatonEnum has no automaton"));
}

// ===========================================================================
// CompiledAutomaton
// ===========================================================================

Result<CompiledAutomaton> CompiledAutomaton::from_automaton(Automaton automaton) {
    return with_binary(std::move(automaton), false, true, false);
}

Result<CompiledAutomaton> CompiledAutomaton::create(Automaton automaton, bool finite, bool simplify) {
    return with_binary(std::move(automaton), finite, simplify, false);
}

Result<int32_t> CompiledAutomaton::find_sink_state(const Automaton &automaton) {
    const int32_t num_states = automaton.get_num_states();
    Transition t;
    for (int32_t s = 0; s < num_states; ++s) {
        if (!automaton.is_accept(s)) {
            continue;
        }
        const int32_t count = automaton.init_transition(s, t);
        for (int32_t i = 0; i < count; ++i) {
            auto next = automaton.get_next_transition(t);
            if (!next) {
                return std::unexpected(std::move(next.error()));
            }
            if (t.dest == s && t.min == 0 && t.max == 0xff) {
                return s;
            }
        }
    }
    return -1;
}

Result<CompiledAutomaton> CompiledAutomaton::with_binary(Automaton automaton, bool finite, bool simplify,
                                                        bool is_binary) {
    if (automaton.get_num_states() == 0) {
        Automaton fresh;
        static_cast<void>(fresh.create_state());
        automaton = std::move(fresh);
    }

    // simplify requires a DFA
    if (simplify && automaton.is_deterministic()) {
        // Test whether the automaton is a "simple" form and, if so, don't create
        // a run automaton.  Note that on a large automaton these tests could be
        // costly.
        auto is_empty = Operations::is_empty(automaton);
        if (!is_empty) {
            return std::unexpected(std::move(is_empty.error()));
        }
        if (is_empty.value()) {
            CompiledAutomaton compiled;
            compiled.type_ = AutomatonType::kNone;
            compiled.finite_ = true;
            compiled.sink_state_ = -1;
            return compiled;
        }

        // NOTE: only approximate, because the automaton may not be minimal.
        auto is_total = is_binary ? Operations::is_total_with_range(automaton, 0, 0xff) : Operations::is_total(automaton);
        if (!is_total) {
            return std::unexpected(std::move(is_total.error()));
        }
        if (is_total.value()) {
            // matches all possible strings
            CompiledAutomaton compiled;
            compiled.type_ = AutomatonType::kAll;
            compiled.finite_ = true;
            compiled.sink_state_ = -1;
            return compiled;
        }

        auto singleton = Operations::get_singleton(automaton);
        if (!singleton) {
            return std::unexpected(std::move(singleton.error()));
        }
        if (singleton->has_value()) {
            BytesRef term;
            if (is_binary) {
                const auto codes = (*singleton)->as_slice();
                std::vector<uint8_t> bytes;
                bytes.reserve(codes.size());
                for (const int32_t code : codes) {
                    bytes.push_back(static_cast<uint8_t>(code));
                }
                term = BytesRef::from_slice(std::move(bytes), 0, codes.size());
            } else {
                std::string utf8;
                for (const int32_t code_point : (*singleton)->as_slice()) {
                    auto appended = StringHelper::utf8_append(utf8, code_point);
                    if (!appended) {
                        return std::unexpected(std::move(appended.error()));
                    }
                }
                term = BytesRef::from_string(utf8);
            }
            CompiledAutomaton compiled;
            compiled.type_ = AutomatonType::kSingle;
            compiled.term_ = std::move(term);
            compiled.finite_ = true;
            compiled.sink_state_ = -1;
            return compiled;
        }
    }

    const bool automaton_is_deterministic = automaton.is_deterministic();

    // The automaton the run automaton is built from, in bytes.
    if (!is_binary) {
        // The incoming automaton is Unicode; convert to UTF-8 to match what is in
        // the index.
        auto converted = UTF32ToUTF8::convert(automaton);
        if (!converted) {
            return std::unexpected(std::move(converted.error()));
        }
        if (converted->has_value()) {
            automaton = std::move(**converted);
        }
    }

    // Compute a common suffix for infinite DFAs; this is an optimization for
    // "leading wildcard", so don't burn cycles on it when the DFA is finite or
    // largeish.
    std::optional<BytesRef> common_suffix;
    if (!finite && static_cast<size_t>(automaton.get_num_states()) +
                    static_cast<size_t>(automaton.get_num_transitions()) <= 1000) {
        auto suffix = Operations::get_common_suffix_bytes_ref(automaton);
        if (!suffix) {
            return std::unexpected(std::move(suffix.error()));
        }
        if (suffix->length() > 0) {
            common_suffix = std::move(suffix.value());
        }
    }

    if (!automaton_is_deterministic && !automaton.is_deterministic()) {
        // Neither the codepoint automaton nor its UTF-8 conversion is a DFA: run
        // the NFA directly and determinize lazily.
        auto nfa = NFARunAutomaton::create_with_alphabet_size(std::move(automaton), 0xff);
        if (!nfa) {
            return std::unexpected(std::move(nfa.error()));
        }
        CompiledAutomaton compiled;
        compiled.type_ = AutomatonType::kNormal;
        compiled.nfa_run_automaton_ = std::move(nfa.value());
        compiled.common_suffix_ = std::move(common_suffix);
        compiled.finite_ = finite;
        compiled.sink_state_ = -1;
        return compiled;
    }

    // We already had a DFA (or threw an error); according to Mike, UTF32ToUTF8
    // won't "blow up".
    auto determinized = Operations::determinize(automaton, static_cast<size_t>(std::numeric_limits<int32_t>::max()));
    if (!determinized) {
        return std::unexpected(std::move(determinized.error()));
    }
    if (determinized->has_value()) {
        automaton = std::move(**determinized);
    }

    auto run_automaton = ByteRunAutomaton::create_with_binary(std::move(automaton), true);
    if (!run_automaton) {
        return std::unexpected(std::move(run_automaton.error()));
    }
    auto sink = find_sink_state(run_automaton->base().automaton());
    if (!sink) {
        return std::unexpected(std::move(sink.error()));
    }

    CompiledAutomaton compiled;
    compiled.type_ = AutomatonType::kNormal;
    compiled.run_automaton_ = std::move(run_automaton.value());
    compiled.common_suffix_ = std::move(common_suffix);
    compiled.finite_ = finite;
    compiled.sink_state_ = sink.value();
    return compiled;
}

Result<AutomatonEnum> CompiledAutomaton::get_automaton() const {
    if (run_automaton_.has_value() && nfa_run_automaton_.has_value()) {
        return std::unexpected(
                LuceneError::illegal_state("both run_automaton and nfa_run_automaton are non-null"));
    }
    if (run_automaton_.has_value()) {
        return AutomatonEnum(*run_automaton_);
    }
    if (nfa_run_automaton_.has_value()) {
        return AutomatonEnum(*nfa_run_automaton_);
    }
    return std::unexpected(LuceneError::illegal_state("neither run_automaton nor nfa_run_automaton is set"));
}

Result<BytesRef> CompiledAutomaton::add_tail(int32_t state, BytesRefBuilder &term, size_t idx, int32_t lead_label) {
    if (!run_automaton_) {
        return std::unexpected(LuceneError::illegal_state("normal compiled automaton is missing its DFA"));
    }
    const Automaton &automaton = run_automaton_->base().automaton();

    std::optional<int32_t> max_index;
    const int32_t num_transitions = automaton.init_transition(state, transition_);
    for (int32_t i = 0; i < num_transitions; ++i) {
        auto next = automaton.get_next_transition(transition_);
        if (!next) {
            return std::unexpected(std::move(next.error()));
        }
        if (transition_.min < lead_label) {
            max_index = i;
        } else {
            // Transitions are always sorted.
            break;
        }
    }
    if (!max_index.has_value()) {
        return std::unexpected(LuceneError::illegal_state("no transition below the lead label exists in add_tail"));
    }

    automaton.get_transition(state, max_index.value(), transition_);
    // Append floor label.
    const int32_t floor_label = transition_.max > lead_label - 1 ? lead_label - 1 : transition_.max;

    auto grown = term.grow(idx + 1);
    if (!grown) {
        return std::unexpected(std::move(grown.error()));
    }
    term.set_byte_at(idx, static_cast<uint8_t>(floor_label));
    state = transition_.dest;
    ++idx;

    while (true) {
        const int32_t count = automaton.get_num_transitions_with_state(state);
        if (count == 0) {
            term.set_length(idx);
            return term.get_bytes_owner();
        }
        automaton.get_transition(state, count - 1, transition_);
        grown = term.grow(idx + 1);
        if (!grown) {
            return std::unexpected(std::move(grown.error()));
        }
        term.set_byte_at(idx, static_cast<uint8_t>(transition_.max));
        state = transition_.dest;
        ++idx;
    }
}

Result<std::optional<BytesRef>> CompiledAutomaton::floor(const BytesRef &input, BytesRefBuilder &output) {
    if (!run_automaton_) {
        return std::unexpected(LuceneError::illegal_state("normal compiled automaton is missing its DFA"));
    }
    const Automaton &automaton = run_automaton_->base().automaton();
    int32_t state = 0;

    // Special case: empty string
    if (input.length() == 0) {
        auto accepted = run_automaton_->is_accept(state);
        if (!accepted) {
            return std::unexpected(std::move(accepted.error()));
        }
        if (!accepted.value()) {
            return std::optional<BytesRef>{};
        }
        output.clear();
        return std::optional<BytesRef>(output.get_bytes_owner());
    }

    const auto bytes = input.as_slice();
    size_t idx = 0;
    std::vector<int32_t> stack;
    stack.reserve(input.length());

    while (true) {
        int32_t label = static_cast<int32_t>(bytes[idx]);
        auto stepped = run_automaton_->step(state, label);
        if (!stepped) {
            return std::unexpected(std::move(stepped.error()));
        }
        int32_t next_state = stepped.value();

        if (idx == input.length() - 1) {
            bool is_accept = false;
            if (next_state != -1) {
                auto accepted = run_automaton_->is_accept(next_state);
                if (!accepted) {
                    return std::unexpected(std::move(accepted.error()));
                }
                is_accept = accepted.value();
            }
            if (is_accept) {
                auto grown = output.grow(idx + 1);
                if (!grown) {
                    return std::unexpected(std::move(grown.error()));
                }
                output.set_byte_at(idx, static_cast<uint8_t>(label));
                output.set_length(input.length());
                return std::optional<BytesRef>(output.get_bytes_owner());
            }
            next_state = -1;
        }

        if (next_state == -1) {
            // Pop back to a state that has a transition <= our label.
            while (true) {
                const int32_t num_transitions = automaton.get_num_transitions_with_state(state);
                if (num_transitions == 0) {
                    output.set_length(idx);
                    return std::optional<BytesRef>(output.get_bytes_owner());
                }
                automaton.get_transition(state, 0, transition_);
                if (label - 1 < transition_.min) {
                    auto accepted = run_automaton_->is_accept(state);
                    if (!accepted) {
                        return std::unexpected(std::move(accepted.error()));
                    }
                    if (accepted.value()) {
                        output.set_length(idx);
                        return std::optional<BytesRef>(output.get_bytes_owner());
                    }
                    if (stack.empty()) {
                        return std::optional<BytesRef>{};
                    }
                    state = stack.back();
                    stack.pop_back();
                    --idx;
                    label = static_cast<int32_t>(bytes[idx]);
                } else {
                    break;
                }
            }
            auto tail = add_tail(state, output, idx, label);
            if (!tail) {
                return std::unexpected(std::move(tail.error()));
            }
            return std::optional<BytesRef>(std::move(tail.value()));
        }

        auto grown = output.grow(idx + 1);
        if (!grown) {
            return std::unexpected(std::move(grown.error()));
        }
        output.set_byte_at(idx, static_cast<uint8_t>(label));
        stack.push_back(state);
        state = next_state;
        ++idx;
    }
}

bool CompiledAutomaton::operator==(const CompiledAutomaton &other) const {
    if (type_ != other.type_) {
        return false;
    }
    switch (type_) {
        case AutomatonType::kSingle:
            return term_ == other.term_;
        case AutomatonType::kNormal:
            if (run_automaton_.has_value() != other.run_automaton_.has_value()) {
                return false;
            }
            if (run_automaton_.has_value() && !(run_automaton_->base() == other.run_automaton_->base())) {
                return false;
            }
            if (nfa_run_automaton_.has_value() != other.nfa_run_automaton_.has_value()) {
                return false;
            }
            if (nfa_run_automaton_.has_value()) {
                return nfa_run_automaton_->automaton() == other.nfa_run_automaton_->automaton();
            }
            return true;
        case AutomatonType::kAll:
        case AutomatonType::kNone:
            return true;
    }
    return false;
}


VoidResult CompiledAutomaton::visit(QueryVisitor &visitor, const Query &query, std::string_view field) const {
    if (!visitor.accept_field(field)) {
        return {};
    }
    switch (type_) {
        case AutomatonType::kNone:
            // No term is accepted, so there is nothing to tell the visitor.
            return {};
        case AutomatonType::kAll:
            return visitor.consume_terms_matching(
                    query, field, []() -> Result<std::optional<ByteRunAutomaton>> {
                        auto automaton = Automata::make_any_string();
                        if (!automaton.has_value()) {
                            return std::unexpected(std::move(automaton.error()));
                        }
                        auto run = ByteRunAutomaton::create(std::move(automaton.value()));
                        if (!run.has_value()) {
                            return std::unexpected(std::move(run.error()));
                        }
                        return std::optional<ByteRunAutomaton>(std::move(run.value()));
                    });
        case AutomatonType::kSingle: {
            if (!term_.has_value()) {
                return err(LuceneError::illegal_state("a singleton compiled automaton is missing its term"));
            }
            const std::array<Term, 1> terms{Term(std::string(field), term_.value())};
            return visitor.consume_terms(query, terms);
        }
        case AutomatonType::kNormal:
            return visitor.consume_terms_matching(
                    query, field, [this]() -> Result<std::optional<ByteRunAutomaton>> {
                        if (!run_automaton_.has_value()) {
                            return err(LuceneError::illegal_state(
                                    "a normal compiled automaton is missing its DFA"));
                        }
                        return std::optional<ByteRunAutomaton>(run_automaton_.value());
                    });
    }
    return {};
}

} // namespace lucene
