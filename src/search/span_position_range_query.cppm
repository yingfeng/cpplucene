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

export module lucene_core:span_position_range_query;

import :index_searcher;
import :query;
import :span_position_check_query;
import :span_query;
import :spans;
import :types;

import std;

namespace lucene {

/// Keeps the wrapped spans whose `[start, end)` fits inside `[start, end)`.
///
/// Port of `SpanPositionRangeQuery` (`queries/span/span_position_range_query.rs`).
/// A span that starts at or after `end` cannot be followed by an acceptable one
/// in the same document, so it ends the document's enumeration.
export class SpanPositionRangeQuery : public SpanPositionCheckQuery {
public:
    SpanPositionRangeQuery(std::shared_ptr<SpanQuery> match, int32_t start, int32_t end);

    [[nodiscard]] int32_t start() const noexcept { return start_; }
    [[nodiscard]] int32_t end() const noexcept { return end_; }

    [[nodiscard]] Result<SpanAcceptStatus> accept_position(Spans &spans) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

protected:
    [[nodiscard]] std::shared_ptr<SpanQuery> with_match(std::shared_ptr<SpanQuery> match) const override;

    int32_t start_;
    int32_t end_;
};

} // namespace lucene
