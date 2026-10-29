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

module lucene_core:near_spans_ordered.impl;

import :near_spans_ordered;

import :conjunction_spans;
import :spans;
import :types;

import std;

namespace lucene {

namespace {

/// Advances `spans` until its start position is at or past `position`, and
/// returns the start position it stopped on.
[[nodiscard]] Result<int32_t> advance_position(Spans &spans, int32_t position) {
    while (spans.start_position() < position) {
        auto next = spans.next_start_position();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
    }
    return spans.start_position();
}

} // namespace

NearSpansOrdered::NearSpansOrdered(int32_t allowed_slop, std::vector<std::unique_ptr<Spans>> sub_spans)
    : ConjunctionSpans(std::move(sub_spans)), allowed_slop_(allowed_slop) {
    at_first_in_current_doc_ = true;
}

Result<bool> NearSpansOrdered::two_phase_current_doc_matches() {
    one_exhausted_in_current_doc_ = false;
    while (true) {
        auto next = sub_spans_.front()->next_start_position();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        if (next.value() == NO_MORE_POSITIONS || one_exhausted_in_current_doc_) {
            return false;
        }
        auto ordered = stretch_to_order();
        if (!ordered.has_value()) {
            return std::unexpected(std::move(ordered.error()));
        }
        if (ordered.value() && match_width_ <= allowed_slop_) {
            at_first_in_current_doc_ = true;
            return true;
        }
    }
}

Result<int32_t> NearSpansOrdered::next_start_position() {
    if (at_first_in_current_doc_) {
        at_first_in_current_doc_ = false;
        return match_start_;
    }

    one_exhausted_in_current_doc_ = false;
    while (true) {
        auto next = sub_spans_.front()->next_start_position();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        if (next.value() == NO_MORE_POSITIONS || one_exhausted_in_current_doc_) {
            match_start_ = NO_MORE_POSITIONS;
            match_end_ = NO_MORE_POSITIONS;
            return match_start_;
        }
        auto ordered = stretch_to_order();
        if (!ordered.has_value()) {
            return std::unexpected(std::move(ordered.error()));
        }
        if (ordered.value() && match_width_ <= allowed_slop_) {
            return match_start_;
        }
    }
}

Result<bool> NearSpansOrdered::stretch_to_order() {
    Spans *previous = sub_spans_.front().get();
    match_start_ = previous->start_position();
    match_width_ = 0;
    for (size_t i = 1; i < sub_spans_.size(); ++i) {
        Spans &spans = *sub_spans_[i];
        auto position = advance_position(spans, previous->end_position());
        if (!position.has_value()) {
            return std::unexpected(std::move(position.error()));
        }
        if (position.value() == NO_MORE_POSITIONS) {
            one_exhausted_in_current_doc_ = true;
            return false;
        }
        match_width_ += spans.start_position() - previous->end_position();
        previous = &spans;
    }
    match_end_ = sub_spans_.back()->end_position();
    return true;
}

VoidResult NearSpansOrdered::collect(SpanCollector &collector) {
    for (const std::unique_ptr<Spans> &spans : sub_spans_) {
        if (VoidResult collected = spans->collect(collector); !collected.has_value()) {
            return collected;
        }
    }
    return {};
}

} // namespace lucene
