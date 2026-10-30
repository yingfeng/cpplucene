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

module lucene_core:token_stream_to_term_automaton_query.impl;

import :token_stream_to_term_automaton_query;

import std;

import :attribute_source;
import :attributes;
import :error;
import :term_automaton_query;
import :token_stream;
import :types;

namespace lucene {

Result<TermAutomatonQuery> TokenStreamToTermAutomatonQuery::to_query(std::string field, TokenStream &input) const {
    if (auto reset = input.reset(); !reset.has_value()) {
        return std::unexpected(std::move(reset.error()));
    }

    TermAutomatonQuery query(std::move(field));

    AttributeSource &attributes = input.attributes();
    auto *position_increment = attributes.add_attribute<PositionIncrementAttribute>();
    auto *position_length = attributes.add_attribute<PositionLengthAttribute>();
    auto *char_term = attributes.add_attribute<CharTermAttribute>();

    int32_t pos = -1;
    int32_t state = -1;
    while (true) {
        auto has_next = input.increment_token();
        if (!has_next.has_value()) {
            return std::unexpected(std::move(has_next.error()));
        }
        if (!has_next.value()) {
            break;
        }

        int32_t pos_inc = static_cast<int32_t>(position_increment->position_increment());
        if (!preserve_position_increments_ && pos_inc > 1) {
            pos_inc = 1;
        }
        if (pos_inc > 1) {
            return err(LuceneError::illegal_argument("cannot handle holes; to accept any term, use '*' term"));
        }
        if (pos_inc > 0) {
            // A new node.
            pos += pos_inc;
        }

        const int32_t end_pos = pos + static_cast<int32_t>(position_length->position_length());
        // States are numbered by position, so create the ones the token needs.
        while (state < end_pos) {
            state = query.create_state();
        }

        const std::string_view term = char_term->text();
        if (term.size() == 1 && term.front() == '*') {
            if (auto added = query.add_any_transition(pos, end_pos); !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
        } else {
            const auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(term.data()), term.size());
            if (auto added = query.add_transition_bytes(pos, end_pos, bytes); !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
        }
    }

    if (auto finished = input.end(); !finished.has_value()) {
        return std::unexpected(std::move(finished.error()));
    }

    if (state < 0) {
        return err(LuceneError::illegal_argument("the token stream produced no tokens"));
    }
    query.set_accept(state, true);
    if (auto built = query.finish(); !built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    return query;
}

} // namespace lucene
