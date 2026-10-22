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

module lucene_core:character_run_automaton.impl;

import :character_run_automaton;

import :automaton;
import :error;
import :run_automaton;
import :string_helper;
import :types;

import std;

namespace lucene {

Result<CharacterRunAutomaton> CharacterRunAutomaton::create(Automaton automaton) {
    auto base = RunAutomaton::create(std::move(automaton), static_cast<size_t>(MAX_CODE_POINT) + 1);
    if (!base) {
        return err(std::move(base.error()));
    }
    return CharacterRunAutomaton(std::move(base.value()));
}

Result<bool> CharacterRunAutomaton::run_str(std::string_view s) const {
    int32_t state = 0;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t num_bytes = 0;
        auto code_point = StringHelper::utf8_code_point_at(s, pos, num_bytes);
        if (!code_point) {
            return err(std::move(code_point.error()));
        }
        auto next_state = base_.step(state, code_point.value());
        if (!next_state) {
            return err(std::move(next_state.error()));
        }
        state = next_state.value();
        if (state == -1) {
            return false;
        }
        pos += num_bytes;
    }
    return base_.is_accept(state);
}

Result<bool> CharacterRunAutomaton::run_chars(std::span<const uint16_t> chars, size_t offset, size_t length) const {
    if (offset > chars.size() || length > chars.size() - offset) {
        return err(LuceneError::illegal_argument(
                std::format("slice [{}, {}) is out of bounds for {} UTF-16 code units", offset, offset + length,
                            chars.size())));
    }

    int32_t state = 0;
    size_t i = offset;
    const size_t end = offset + length;
    while (i < end) {
        const uint32_t unit = chars[i];
        int32_t code_point = 0;
        if (unit < 0xD800u || unit > 0xDFFFu) {
            code_point = static_cast<int32_t>(unit);
            i += 1;
        } else if (unit <= 0xDBFFu && i + 1 < end && chars[i + 1] >= 0xDC00u && chars[i + 1] <= 0xDFFFu) {
            // A high surrogate followed by a low surrogate.
            code_point = static_cast<int32_t>(0x10000u + ((unit - 0xD800u) << 10) + (chars[i + 1] - 0xDC00u));
            i += 2;
        } else {
            // An unpaired surrogate is not a character.
            return false;
        }

        auto next_state = base_.step(state, code_point);
        if (!next_state) {
            return err(std::move(next_state.error()));
        }
        state = next_state.value();
        if (state == -1) {
            return false;
        }
    }

    return base_.is_accept(state);
}

} // namespace lucene
