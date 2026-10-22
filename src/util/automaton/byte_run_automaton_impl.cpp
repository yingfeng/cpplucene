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

module lucene_core:byte_run_automaton.impl;

import :byte_run_automaton;

import :automaton;
import :error;
import :operations;
import :run_automaton;
import :types;
import :utf32_to_utf8;

import std;

namespace lucene {

Result<ByteRunAutomaton> ByteRunAutomaton::create(Automaton a) {
    return create_with_binary(std::move(a), false);
}

Result<ByteRunAutomaton> ByteRunAutomaton::create_with_binary(Automaton a, bool is_binary) {
    Automaton automaton = std::move(a);
    if (!is_binary) {
        if (!automaton.is_deterministic()) {
            return err(LuceneError::illegal_argument("Automaton must be deterministic"));
        }
        auto converted = UTF32ToUTF8::convert(automaton);
        if (!converted) {
            return err(std::move(converted.error()));
        }
        if (converted.value().has_value()) {
            automaton = std::move(converted.value().value());
        }
        auto deterministic = Operations::determinize(automaton, std::numeric_limits<int32_t>::max());
        if (!deterministic) {
            return err(std::move(deterministic.error()));
        }
        if (deterministic.value().has_value()) {
            automaton = std::move(deterministic.value().value());
        }
    }

    auto base = RunAutomaton::create(std::move(automaton), 256);
    if (!base) {
        return err(std::move(base.error()));
    }
    return ByteRunAutomaton(std::move(base.value()));
}

Result<int32_t> ByteRunAutomaton::step(int32_t state, int32_t c) { return base_.step(state, c); }

Result<bool> ByteRunAutomaton::is_accept(int32_t state) const { return base_.is_accept(state); }

} // namespace lucene
