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

module lucene_core:term_spans.impl;

import :term_spans;

import :doc_id_set_iterator;
import :postings_enum;
import :spans;
import :term;
import :types;

import std;

namespace lucene {

TermSpans::TermSpans(std::unique_ptr<PostingsEnum> postings, Term term, float positions_cost)
    : postings_(std::move(postings)), term_(std::move(term)), positions_cost_(positions_cost) {
    debug_check(positions_cost_ > 0.0F, "a term spans needs a positive positions cost");
}

Result<int32_t> TermSpans::next_doc() {
    auto advanced = postings_->next_doc();
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    doc_ = advanced.value();
    if (doc_ != NO_MORE_DOCS) {
        auto frequency = postings_->freq();
        if (!frequency.has_value()) {
            return std::unexpected(std::move(frequency.error()));
        }
        freq_ = frequency.value();
        count_ = 0;
    }
    position_ = -1;
    return doc_;
}

Result<int32_t> TermSpans::advance(int32_t target) {
    auto advanced = postings_->advance(target);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    doc_ = advanced.value();
    if (doc_ != NO_MORE_DOCS) {
        auto frequency = postings_->freq();
        if (!frequency.has_value()) {
            return std::unexpected(std::move(frequency.error()));
        }
        freq_ = frequency.value();
        count_ = 0;
    }
    position_ = -1;
    return doc_;
}

Result<int64_t> TermSpans::cost() const { return postings_->cost(); }

Result<int32_t> TermSpans::next_start_position() {
    if (count_ == freq_) {
        position_ = NO_MORE_POSITIONS;
        return position_;
    }
    auto next = postings_->next_position();
    if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
    }
    debug_check(next.value() >= position_, "the positions of a document must increase");
    position_ = next.value();
    ++count_;
    return position_;
}

int32_t TermSpans::end_position() const {
    if (position_ == -1) {
        return -1;
    }
    return position_ != NO_MORE_POSITIONS ? position_ + 1 : NO_MORE_POSITIONS;
}

VoidResult TermSpans::collect(SpanCollector &collector) {
    return collector.collect_leaf(*postings_, position_, term_);
}

} // namespace lucene
