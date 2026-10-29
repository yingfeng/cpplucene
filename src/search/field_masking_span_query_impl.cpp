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

module lucene_core:field_masking_span_query.impl;

import :field_masking_span_query;

import :error;
import :index_searcher;
import :occur;
import :query;
import :query_visitor;
import :score_mode;
import :span_query;
import :types;
import :weight;

import std;

namespace lucene {

FieldMaskingSpanQuery::FieldMaskingSpanQuery(std::shared_ptr<SpanQuery> masked_query, std::string masked_field)
    : masked_query_(std::move(masked_query)), field_(std::move(masked_field)) {
    debug_check(masked_query_ != nullptr, "a masking query needs a wrapped query");
    debug_check(!field_.empty(), "a masking query needs a field name");
}

Result<std::unique_ptr<Weight>> FieldMaskingSpanQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                     float boost) const {
    // The masked query keeps its own field for the postings and the scoring; the
    // mask only changes what the surrounding span query sees.
    return masked_query_->create_weight(searcher, score_mode, boost);
}

Result<std::shared_ptr<Query>> FieldMaskingSpanQuery::rewrite(const IndexSearcher &searcher) const {
    auto rewritten = masked_query_->rewrite(searcher);
    if (!rewritten.has_value()) {
        return std::unexpected(std::move(rewritten.error()));
    }
    if (rewritten.value() == nullptr) {
        return std::shared_ptr<Query>{};
    }
    auto *span_query = dynamic_cast<SpanQuery *>(rewritten.value().get());
    if (span_query == nullptr) {
        return err(LuceneError::illegal_argument("a masked query must stay a span query"));
    }
    return std::make_shared<FieldMaskingSpanQuery>(std::shared_ptr<SpanQuery>(rewritten.value(), span_query), field_);
}

VoidResult FieldMaskingSpanQuery::visit(QueryVisitor &visitor) const {
    if (!visitor.accept_field(field_)) {
        return {};
    }
    DefaultQueryVisitor sub_visitor = visitor.get_sub_visitor(Occur::Must, *this);
    return masked_query_->visit(sub_visitor);
}

Result<std::string> FieldMaskingSpanQuery::to_string(std::string_view field) const {
    auto masked_text = masked_query_->to_string(field);
    if (!masked_text.has_value()) {
        return std::unexpected(std::move(masked_text.error()));
    }
    return "mask(" + masked_text.value() + ") as " + field_;
}

bool FieldMaskingSpanQuery::equals(const Query &other) const {
    if (typeid(*this) != typeid(other)) {
        return false;
    }
    const auto &query = dynamic_cast<const FieldMaskingSpanQuery &>(other);
    return field_ == query.field_ && masked_query_->equals(*query.masked_query_);
}

size_t FieldMaskingSpanQuery::hash() const {
    return 0x9E3779B9U ^ masked_query_->hash() ^ std::hash<std::string>{}(field_);
}

} // namespace lucene
