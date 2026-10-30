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

module lucene_core:exact_phrase_matcher.impl;

import :exact_phrase_matcher;

import std;

import :error;
import :phrase_matcher;
import :postings_enum;
import :types;

namespace lucene {

ExactPhraseMatcher::ExactPhraseMatcher(std::vector<PostingsAndFreq> postings, float match_cost)
        : PhraseMatcherBase(std::move(postings)), match_cost_(match_cost) {
    slots_.reserve(num_postings());
    for (const PostingsAndFreq &entry : postings_) {
        slots_.push_back(Slot{.offset = entry.position});
    }
}

int32_t ExactPhraseMatcher::start_position() const { return slots_[0].pos; }

int32_t ExactPhraseMatcher::end_position() const { return slots_[slots_.size() - 1].pos; }

Result<int32_t> ExactPhraseMatcher::start_offset() const {
    // Offsets are only loaded when a matches iterator asks for them.
    PostingsEnum &postings = *postings_[0].postings;
    return postings.start_offset();
}

Result<int32_t> ExactPhraseMatcher::end_offset() const {
    PostingsEnum &postings = *postings_[postings_.size() - 1].postings;
    return postings.end_offset();
}

} // namespace lucene
