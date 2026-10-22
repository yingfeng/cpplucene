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

module lucene_core:finite_strings_iterator.impl;

import :finite_strings_iterator;

import :automaton;
import :debug;
import :error;
import :fixed_bit_set;
import :ints_ref;
import :transition;
import :types;

import std;

namespace lucene {

void FiniteStringsIterator::PathNode::reset_state(const Automaton &a, int32_t state) {
    debug_check(a.get_num_transitions_with_state(state) != 0, "a reset state has transitions");
    this->state = state;
    transition = 0;
    a.get_transition(state, 0, t);
    label = t.min;
    to = t.dest;
}

int32_t FiniteStringsIterator::PathNode::next_label(const Automaton &a) {
    if (label > t.max) {
        // The current transition's labels are exhausted: move to the next one.
        transition += 1;
        if (transition >= a.get_num_transitions_with_state(state)) {
            label = -1;
            return -1;
        }
        a.get_transition(state, transition, t);
        label = t.min;
        to = t.dest;
    }
    const int32_t result = label;
    label += 1;
    return result;
}

FiniteStringsIterator::FiniteStringsIterator(const Automaton &a) : FiniteStringsIterator(a, 0, -1) {}

FiniteStringsIterator::FiniteStringsIterator(const Automaton &a, int32_t start_state, int32_t end_state)
    : a_(a),
      end_state_(end_state),
      path_states_(static_cast<size_t>(a.get_num_states())),
      nodes_(1, PathNode{}),
      emit_empty_string_(a.is_accept(start_state)) {
    if (a.get_num_states() > start_state && a.get_num_transitions_with_state(start_state) > 0) {
        path_states_.set(static_cast<size_t>(start_state));
        nodes_[0].reset_state(a, start_state);
        (void)string_.append(start_state);
    }
}

void FiniteStringsIterator::grow_stack(size_t depth) {
    if (nodes_.size() == depth) {
        nodes_.emplace_back();
    }
}

Result<std::optional<IntsRef>> FiniteStringsIterator::next() {
    // The empty string is special cased, as usual.
    if (emit_empty_string_) {
        emit_empty_string_ = false;
        return std::optional<IntsRef>(IntsRef());
    }

    size_t depth = string_.length();

    while (depth > 0) {
        PathNode &node = nodes_[depth - 1];

        const int32_t label = node.next_label(a_);
        if (label != -1) {
            string_.set_int_at(depth - 1, label);

            const int32_t to = node.to;
            if (a_.get_num_transitions_with_state(to) != 0 && to != end_state_) {
                // The destination has outgoing transitions: recurse.
                if (path_states_.get_bit(static_cast<size_t>(to))) {
                    return err(LuceneError::illegal_argument("automaton has cycles"));
                }
                path_states_.set(static_cast<size_t>(to));
                grow_stack(depth);
                nodes_[depth].reset_state(a_, to);
                depth += 1;
                string_.set_length(depth);
                if (auto grown = string_.grow(depth); !grown) {
                    return err(std::move(grown.error()));
                }
            } else if (end_state_ == to || a_.is_accept(to)) {
                // The transition leads to an accept state: yield the string.
                return std::optional<IntsRef>(string_.to_ints_ref());
            }
        } else {
            // No more transitions leave this state: pop back to the previous one.
            const int32_t state = node.state;
            debug_check(path_states_.get_bit(static_cast<size_t>(state)), "the state is on the path");
            (void)path_states_.clear(static_cast<size_t>(state));

            depth -= 1;
            string_.set_length(depth);

            if (a_.is_accept(state)) {
                return std::optional<IntsRef>(string_.to_ints_ref());
            }
        }
        depth = string_.length();
    }

    // Iteration finished.
    return std::optional<IntsRef>{};
}

} // namespace lucene
