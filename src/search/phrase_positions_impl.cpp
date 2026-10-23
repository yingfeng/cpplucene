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

module lucene_core:phrase_positions.impl;

import :phrase_positions;

import std;

import :error;
import :postings_enum;
import :types;

namespace lucene {

PhrasePositions::PhrasePositions(size_t postings_idx, int32_t offset, size_t ord)
        : postings_idx_(postings_idx), offset_(offset), ord_(ord) {}

VoidResult PhrasePositions::first_position(PostingsEnum &postings) {
    auto frequency = postings.freq();
    if (!frequency.has_value()) {
        return std::unexpected(std::move(frequency.error()));
    }
    count_ = frequency.value();
    auto advanced = next_position(postings);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    return {};
}

Result<bool> PhrasePositions::next_position(PostingsEnum &postings) {
    const int32_t count = count_;
    count_ = count - 1;
    if (count <= 0) {
        return false;
    }
    auto position = postings.next_position();
    if (!position.has_value()) {
        return std::unexpected(std::move(position.error()));
    }
    position_ = position.value() - offset_;
    return true;
}

} // namespace lucene
