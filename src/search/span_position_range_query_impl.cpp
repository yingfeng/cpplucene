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

module lucene_core:span_position_range_query.impl;

import :span_position_range_query;

import :filter_spans;
import :query;
import :span_position_check_query;
import :span_query;
import :spans;
import :types;

import std;

namespace lucene {

SpanPositionRangeQuery::SpanPositionRangeQuery(std::shared_ptr<SpanQuery> match, int32_t start, int32_t end)
    : SpanPositionCheckQuery(std::move(match)), start_(start), end_(end) {}

Result<SpanAcceptStatus> SpanPositionRangeQuery::accept_position(Spans &spans) const {
    if (spans.start_position() >= end_) {
        // Positions only increase, so nothing further in this document can fit.
        return SpanAcceptStatus::kNoMoreInCurrentDoc;
    }
    if (spans.start_position() >= start_ && spans.end_position() <= end_) {
        return SpanAcceptStatus::kYes;
    }
    return SpanAcceptStatus::kNo;
}

Result<std::string> SpanPositionRangeQuery::to_string(std::string_view field) const {
    auto match_text = match_->to_string(field);
    if (!match_text.has_value()) {
        return std::unexpected(std::move(match_text.error()));
    }
    return "spanPosRange(" + match_text.value() + ", " + std::to_string(start_) + ", " + std::to_string(end_) + ")";
}

bool SpanPositionRangeQuery::equals(const Query &other) const {
    if (!SpanPositionCheckQuery::equals(other)) {
        return false;
    }
    const auto &query = dynamic_cast<const SpanPositionRangeQuery &>(other);
    return start_ == query.start_ && end_ == query.end_;
}

size_t SpanPositionRangeQuery::hash() const {
    size_t result = SpanPositionCheckQuery::hash() ^ static_cast<size_t>(end_);
    result = result * 127 ^ static_cast<size_t>(start_);
    return result;
}

std::shared_ptr<SpanQuery> SpanPositionRangeQuery::with_match(std::shared_ptr<SpanQuery> match) const {
    return std::make_shared<SpanPositionRangeQuery>(std::move(match), start_, end_);
}

} // namespace lucene
