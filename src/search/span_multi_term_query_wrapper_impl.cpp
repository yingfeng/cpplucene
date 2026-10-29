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

module lucene_core:span_multi_term_query_wrapper.impl;

import :span_multi_term_query_wrapper;

import :error;
import :index_searcher;
import :match_no_docs_query;
import :multi_term_query;
import :occur;
import :query;
import :query_visitor;
import :score_mode;
import :span_or_query;
import :span_query;
import :span_term_query;
import :term;
import :types;
import :weight;

import std;

namespace lucene {

SpanMultiTermQueryWrapper::SpanMultiTermQueryWrapper(std::shared_ptr<MultiTermQuery> query)
    : query_(std::move(query)) {
    debug_check(query_ != nullptr, "a span multi term wrapper needs a query");
}

Result<std::unique_ptr<Weight>> SpanMultiTermQueryWrapper::create_weight(const IndexSearcher & /*searcher*/,
                                                                        ScoreMode /*score_mode*/,
                                                                        float /*boost*/) const {
    return err(LuceneError::illegal_argument(
            "a span multi term query has to be rewritten before it can be weighted; call IndexSearcher::rewrite"));
}

Result<std::shared_ptr<Query>> SpanMultiTermQueryWrapper::rewrite(const IndexSearcher &searcher) const {
    auto terms = query_->expanded_terms(searcher);
    if (!terms.has_value()) {
        return std::unexpected(std::move(terms.error()));
    }

    std::vector<std::shared_ptr<SpanQuery>> clauses;
    clauses.reserve(terms.value().size());
    for (const Term &term : terms.value()) {
        clauses.push_back(std::make_shared<SpanTermQuery>(term));
    }
    if (clauses.empty()) {
        return std::shared_ptr<Query>(MatchNoDocsQuery::create("span mult term query expanded to no terms"));
    }

    auto or_query = SpanOrQuery::create(std::move(clauses));
    if (!or_query.has_value()) {
        return std::unexpected(std::move(or_query.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<SpanOrQuery>(std::move(or_query.value())));
}

VoidResult SpanMultiTermQueryWrapper::visit(QueryVisitor &visitor) const {
    if (!visitor.accept_field(get_field())) {
        return {};
    }
    DefaultQueryVisitor sub_visitor = visitor.get_sub_visitor(Occur::Must, *this);
    return query_->visit(sub_visitor);
}

Result<std::string> SpanMultiTermQueryWrapper::to_string(std::string_view field) const {
    auto wrapped = query_->to_string(field);
    if (!wrapped.has_value()) {
        return std::unexpected(std::move(wrapped.error()));
    }
    return "SpanMultiTermQueryWrapper(" + wrapped.value() + ")";
}

bool SpanMultiTermQueryWrapper::equals(const Query &other) const {
    if (typeid(*this) != typeid(other)) {
        return false;
    }
    const auto &query = dynamic_cast<const SpanMultiTermQueryWrapper &>(other);
    return query_->equals(*query.query_);
}

size_t SpanMultiTermQueryWrapper::hash() const { return 0x85EBCA77U * 31 + query_->hash(); }

} // namespace lucene
