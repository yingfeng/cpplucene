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

module lucene_core:span_first_query.impl;

import :span_first_query;

import :filter_spans;
import :query;
import :span_position_range_query;
import :span_query;
import :spans;
import :types;

import std;

namespace lucene {

SpanFirstQuery::SpanFirstQuery(std::shared_ptr<SpanQuery> match, int32_t end)
    : SpanPositionRangeQuery(std::move(match), 0, end) {}

Result<SpanAcceptStatus> SpanFirstQuery::accept_position(Spans &spans) const {
    if (spans.start_position() >= end_) {
        // A later start cannot end before `end` either.
        return SpanAcceptStatus::kNoMoreInCurrentDoc;
    }
    if (spans.end_position() <= end_) {
        return SpanAcceptStatus::kYes;
    }
    return SpanAcceptStatus::kNo;
}

Result<std::string> SpanFirstQuery::to_string(std::string_view field) const {
    auto match_text = match_->to_string(field);
    if (!match_text.has_value()) {
        return std::unexpected(std::move(match_text.error()));
    }
    return "spanFirst(" + match_text.value() + ", " + std::to_string(end_) + ")";
}

std::shared_ptr<SpanQuery> SpanFirstQuery::with_match(std::shared_ptr<SpanQuery> match) const {
    return std::make_shared<SpanFirstQuery>(std::move(match), end_);
}

} // namespace lucene
