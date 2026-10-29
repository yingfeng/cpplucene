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

module lucene_core:near_spans_unordered.impl;

import :near_spans_unordered;

import :conjunction_spans;
import :doc_id_set_iterator;
import :spans;
import :types;

import std;

namespace lucene {

NearSpansUnordered::NearSpansUnordered(int32_t allowed_slop, std::vector<std::unique_ptr<Spans>> sub_spans)
    : ConjunctionSpans(std::move(sub_spans)), allowed_slop_(allowed_slop) {
    at_first_in_current_doc_ = true;
}

bool NearSpansUnordered::positions_ordered(const Spans &spans1, const Spans &spans2) {
    debug_check(spans1.doc_id() == spans2.doc_id(), "both spans must be on the same document");
    const int32_t start1 = spans1.start_position();
    const int32_t start2 = spans2.start_position();
    if (start1 == start2) {
        return spans1.end_position() < spans2.end_position();
    }
    return start1 < start2;
}

Spans &NearSpansUnordered::top() const {
    size_t best = 0;
    for (size_t i = 1; i < sub_spans_.size(); ++i) {
        if (positions_ordered(*sub_spans_[i], *sub_spans_[best])) {
            best = i;
        }
    }
    return *sub_spans_[best];
}

VoidResult NearSpansUnordered::start_document() {
    total_span_length_ = 0;
    max_end_position_ = -1;
    for (const std::unique_ptr<Spans> &spans : sub_spans_) {
        debug_check(spans->start_position() == -1, "a sub span must not be positioned yet");
        auto position = spans->next_start_position();
        if (!position.has_value()) {
            return std::unexpected(std::move(position.error()));
        }
        debug_check(position.value() != NO_MORE_POSITIONS, "a sub span of a matching document has a position");
        max_end_position_ = std::max(max_end_position_, spans->end_position());
        total_span_length_ += spans->end_position() - spans->start_position();
    }
    return {};
}

Result<bool> NearSpansUnordered::next_position() {
    Spans &top_spans = top();
    const int32_t span_length = top_spans.end_position() - top_spans.start_position();
    auto next = top_spans.next_start_position();
    if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
    }
    if (next.value() == NO_MORE_POSITIONS) {
        return false;
    }
    total_span_length_ -= span_length;
    total_span_length_ += top_spans.end_position() - top_spans.start_position();
    max_end_position_ = std::max(max_end_position_, top_spans.end_position());
    return true;
}

bool NearSpansUnordered::at_match() const {
    // The positions that are not covered by a sub span itself are the slop the
    // match costs.
    return (max_end_position_ - top().start_position() - total_span_length_) <= allowed_slop_;
}

Result<bool> NearSpansUnordered::two_phase_current_doc_matches() {
    if (VoidResult started = start_document(); !started.has_value()) {
        return std::unexpected(std::move(started.error()));
    }
    while (true) {
        if (at_match()) {
            at_first_in_current_doc_ = true;
            one_exhausted_in_current_doc_ = false;
            return true;
        }
        auto advanced = next_position();
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (!advanced.value()) {
            return false;
        }
    }
}

Result<int32_t> NearSpansUnordered::next_start_position() {
    if (at_first_in_current_doc_) {
        at_first_in_current_doc_ = false;
        return top().start_position();
    }
    while (true) {
        auto advanced = next_position();
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (!advanced.value()) {
            one_exhausted_in_current_doc_ = true;
            return NO_MORE_POSITIONS;
        }
        if (at_match()) {
            return top().start_position();
        }
    }
}

int32_t NearSpansUnordered::start_position() const {
    if (at_first_in_current_doc_) {
        return -1;
    }
    return one_exhausted_in_current_doc_ ? NO_MORE_POSITIONS : top().start_position();
}

int32_t NearSpansUnordered::end_position() const {
    if (at_first_in_current_doc_) {
        return -1;
    }
    return one_exhausted_in_current_doc_ ? NO_MORE_POSITIONS : max_end_position_;
}

int32_t NearSpansUnordered::width() const { return max_end_position_ - top().start_position(); }

VoidResult NearSpansUnordered::collect(SpanCollector &collector) {
    for (const std::unique_ptr<Spans> &spans : sub_spans_) {
        if (VoidResult collected = spans->collect(collector); !collected.has_value()) {
            return collected;
        }
    }
    return {};
}

} // namespace lucene
