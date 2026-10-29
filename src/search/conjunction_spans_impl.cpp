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

module lucene_core:conjunction_spans.impl;

import :conjunction_spans;

import :conjunction_disi;
import :doc_id_set_iterator;
import :error;
import :spans;
import :types;

import std;

namespace lucene {

ConjunctionSpans::ConjunctionSpans(std::vector<std::unique_ptr<Spans>> sub_spans) : sub_spans_(std::move(sub_spans)) {
    if (sub_spans_.size() < 2) {
        // The constructor cannot report an error, so the caller's precondition is
        // checked with a debug assertion and the conjunction below stays null.
        debug_check(false, "a conjunction of spans needs at least two sub spans");
        return;
    }
    std::vector<DocIdSetIterator *> iterators;
    iterators.reserve(sub_spans_.size());
    for (const std::unique_ptr<Spans> &spans : sub_spans_) {
        iterators.push_back(spans.get());
    }
    auto conjunction = ConjunctionDISI::create(iterators);
    if (conjunction.has_value()) {
        conjunction_ = std::move(conjunction.value());
    }
    at_first_in_current_doc_ = true;
}

Result<int32_t> ConjunctionSpans::next_doc() {
    auto advanced = conjunction_->next_doc();
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    if (advanced.value() == NO_MORE_DOCS) {
        return NO_MORE_DOCS;
    }
    return to_match_doc();
}

Result<int32_t> ConjunctionSpans::advance(int32_t target) {
    auto advanced = conjunction_->advance(target);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    if (advanced.value() == NO_MORE_DOCS) {
        return NO_MORE_DOCS;
    }
    return to_match_doc();
}

Result<int32_t> ConjunctionSpans::to_match_doc() {
    one_exhausted_in_current_doc_ = false;
    while (true) {
        auto matches = two_phase_current_doc_matches();
        if (!matches.has_value()) {
            return std::unexpected(std::move(matches.error()));
        }
        if (matches.value()) {
            return doc_id();
        }
        auto next = conjunction_->next_doc();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        if (next.value() == NO_MORE_DOCS) {
            return NO_MORE_DOCS;
        }
    }
}

} // namespace lucene
