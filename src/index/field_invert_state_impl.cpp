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

module lucene_core:field_invert_state.impl;

import :field_invert_state;

import :index_types;
import :types;

import std;

namespace lucene {

FieldInvertState::FieldInvertState(int32_t index_created_version_major, std::string name, IndexOptions index_options)
    : index_created_version_major_(index_created_version_major),
      name_(std::move(name)),
      index_options_(index_options),
      position_(0) {}

FieldInvertState FieldInvertState::with_states(int32_t index_created_version_major, std::string name,
                                              IndexOptions index_options, int32_t position, int32_t length,
                                              int32_t num_overlap, int32_t offset, int32_t max_term_frequency,
                                              int32_t unique_term_count) {
    FieldInvertState state(index_created_version_major, std::move(name), index_options);
    state.position_ = position;
    state.length_ = length;
    state.num_overlap_ = num_overlap;
    state.offset_ = offset;
    state.max_term_frequency_ = max_term_frequency;
    state.unique_term_count_ = unique_term_count;
    return state;
}

void FieldInvertState::reset() noexcept {
    position_ = -1;
    length_ = 0;
    num_overlap_ = 0;
    offset_ = 0;
    max_term_frequency_ = 0;
    unique_term_count_ = 0;
    last_start_offset_ = 0;
    last_position_ = 0;
}

} // namespace lucene
