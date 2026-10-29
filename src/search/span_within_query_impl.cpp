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

module lucene_core:span_within_query.impl;

import :span_within_query;

import :error;
import :index_searcher;
import :leaf_reader_context;
import :query;
import :span_contain_query;
import :span_query;
import :span_weight;
import :spans;
import :types;

import std;

namespace lucene {

SpanWithinWeight::SpanWithinWeight(const SpanWithinQuery &query, std::unique_ptr<SpanWeight> big_weight,
                                   std::unique_ptr<SpanWeight> little_weight, const IndexSearcher &searcher,
                                   TermStatesMap term_states, float boost)
    : SpanContainWeight(query, std::move(big_weight), std::move(little_weight), searcher, std::move(term_states),
                        boost) {}

Result<std::unique_ptr<Spans>> SpanWithinWeight::get_spans(const LeafReaderContext &context, SpanPostings required) {
    auto operands = prepare_conjunction(context, required);
    if (!operands.has_value()) {
        return std::unexpected(std::move(operands.error()));
    }
    if (operands.value().empty()) {
        return std::unique_ptr<Spans>{};
    }
    // The reported match is the `little` span, which is the second operand.
    return std::unique_ptr<Spans>(std::make_unique<ContainSpans>(std::move(operands.value()), 1, false));
}

SpanWithinQuery::SpanWithinQuery(std::shared_ptr<SpanQuery> big, std::shared_ptr<SpanQuery> little)
    : SpanContainQuery(std::move(big), std::move(little), 0xCC9E2D51U) {}

Result<SpanWithinQuery> SpanWithinQuery::create(std::shared_ptr<SpanQuery> big, std::shared_ptr<SpanQuery> little) {
    if (big == nullptr || little == nullptr) {
        return err(LuceneError::illegal_argument("a span within query needs both operands"));
    }
    if (big->get_field() != little->get_field()) {
        return err(LuceneError::illegal_argument("big and little not same field"));
    }
    return SpanWithinQuery(std::move(big), std::move(little));
}

Result<std::string> SpanWithinQuery::to_string(std::string_view field) const {
    return to_string_with_name(field, "SpanWithin");
}

std::shared_ptr<SpanQuery> SpanWithinQuery::with_parts(std::shared_ptr<SpanQuery> big,
                                                       std::shared_ptr<SpanQuery> little) const {
    return std::make_shared<SpanWithinQuery>(std::move(big), std::move(little));
}

std::unique_ptr<SpanWeight> SpanWithinQuery::build_weight(std::unique_ptr<SpanWeight> big_weight,
                                                          std::unique_ptr<SpanWeight> little_weight,
                                                          const IndexSearcher &searcher, TermStatesMap term_states,
                                                          float boost) const {
    return std::make_unique<SpanWithinWeight>(*this, std::move(big_weight), std::move(little_weight), searcher,
                                              std::move(term_states), boost);
}

} // namespace lucene
