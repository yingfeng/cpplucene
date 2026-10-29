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

export module lucene_core:span_first_query;

import :filter_spans;
import :query;
import :span_position_range_query;
import :span_query;
import :spans;
import :types;

import std;

namespace lucene {

/// Keeps the wrapped spans that end at or before position `end`, i.e. matches at
/// the very beginning of the field.
///
/// Port of `SpanFirstQuery` (`queries/span/span_first_query.rs`), which is a
/// [`SpanPositionRangeQuery`] with `start == 0` whose acceptance test only looks
/// at the end.
export class SpanFirstQuery : public SpanPositionRangeQuery {
public:
    SpanFirstQuery(std::shared_ptr<SpanQuery> match, int32_t end);

    [[nodiscard]] Result<SpanAcceptStatus> accept_position(Spans &spans) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

protected:
    [[nodiscard]] std::shared_ptr<SpanQuery> with_match(std::shared_ptr<SpanQuery> match) const override;
};

} // namespace lucene
