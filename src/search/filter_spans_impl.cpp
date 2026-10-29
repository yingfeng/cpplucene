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

module lucene_core:filter_spans.impl;

import :filter_spans;

import :doc_id_set_iterator;
import :spans;
import :types;

import std;

namespace lucene {

FilterSpans::FilterSpans(std::unique_ptr<Spans> input) : input_(std::move(input)) {
    debug_check(input_ != nullptr, "a filter spans needs a wrapped spans");
}

Result<int32_t> FilterSpans::next_doc() {
    while (true) {
        auto advanced = input_->next_doc();
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (advanced.value() == NO_MORE_DOCS) {
            return NO_MORE_DOCS;
        }
        auto matches = two_phase_current_doc_matches();
        if (!matches.has_value()) {
            return std::unexpected(std::move(matches.error()));
        }
        if (matches.value()) {
            return advanced.value();
        }
    }
}

Result<int32_t> FilterSpans::advance(int32_t target) {
    auto advanced = input_->advance(target);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    int32_t doc = advanced.value();
    while (doc != NO_MORE_DOCS) {
        auto matches = two_phase_current_doc_matches();
        if (!matches.has_value()) {
            return std::unexpected(std::move(matches.error()));
        }
        if (matches.value()) {
            break;
        }
        auto next = input_->next_doc();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        doc = next.value();
    }
    return doc;
}

Result<bool> FilterSpans::two_phase_current_doc_matches() {
    at_first_in_current_doc_ = false;
    auto position = input_->next_start_position();
    if (!position.has_value()) {
        return std::unexpected(std::move(position.error()));
    }
    start_position_ = position.value();
    while (true) {
        auto status = accept(*input_);
        if (!status.has_value()) {
            return std::unexpected(std::move(status.error()));
        }
        switch (status.value()) {
            case SpanAcceptStatus::kYes:
                at_first_in_current_doc_ = true;
                return true;
            case SpanAcceptStatus::kNo: {
                auto next = input_->next_start_position();
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
                start_position_ = next.value();
                if (start_position_ == NO_MORE_POSITIONS) {
                    start_position_ = -1;
                    return false;
                }
                break;
            }
            case SpanAcceptStatus::kNoMoreInCurrentDoc:
                start_position_ = -1;
                return false;
        }
    }
}

Result<int32_t> FilterSpans::next_start_position() {
    if (at_first_in_current_doc_) {
        at_first_in_current_doc_ = false;
        return start_position_;
    }

    while (true) {
        auto position = input_->next_start_position();
        if (!position.has_value()) {
            return std::unexpected(std::move(position.error()));
        }
        start_position_ = position.value();
        if (start_position_ == NO_MORE_POSITIONS) {
            return NO_MORE_POSITIONS;
        }
        auto status = accept(*input_);
        if (!status.has_value()) {
            return std::unexpected(std::move(status.error()));
        }
        switch (status.value()) {
            case SpanAcceptStatus::kYes:
                return start_position_;
            case SpanAcceptStatus::kNo:
                break;
            case SpanAcceptStatus::kNoMoreInCurrentDoc:
                start_position_ = NO_MORE_POSITIONS;
                return NO_MORE_POSITIONS;
        }
    }
}

int32_t FilterSpans::end_position() const {
    if (at_first_in_current_doc_) {
        return -1;
    }
    return start_position_ != NO_MORE_POSITIONS ? input_->end_position() : NO_MORE_POSITIONS;
}

std::string FilterSpans::to_string() const { return "Filter(" + input_->to_string() + ")"; }

} // namespace lucene
