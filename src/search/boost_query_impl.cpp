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

module lucene_core:boost_query.impl;

import :boost_query;

import std;

import :bit_util;
import :constant_score_query;
import :error;
import :index_searcher;
import :match_no_docs_query;
import :query;
import :query_visitor;
import :score_mode;
import :types;
import :weight;

namespace lucene {

BoostQuery::BoostQuery(std::shared_ptr<Query> query, float boost)
        : query_(std::move(query)), boost_(boost) {}

Result<BoostQuery> BoostQuery::create(std::shared_ptr<Query> query, float boost) {
    if (query == nullptr) {
        return err(LuceneError::illegal_argument("the wrapped query must not be null"));
    }
    // `-0.0f` is rejected as well, as the reference implementation does.
    if (!std::isfinite(boost) || boost < 0.0F || (boost == 0.0F && std::signbit(boost))) {
        return err(LuceneError::illegal_argument(std::format("boost must be a positive float, got {:.1f}", boost)));
    }
    return BoostQuery(std::move(query), boost);
}

Result<std::unique_ptr<Weight>> BoostQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                           float boost) const {
    return query_->create_weight(searcher, score_mode, boost_ * boost);
}

Result<std::shared_ptr<Query>> BoostQuery::rewrite(const IndexSearcher &searcher) const {
    auto rewritten = query_->rewrite(searcher);
    if (!rewritten.has_value()) {
        return std::unexpected(std::move(rewritten.error()));
    }
    std::shared_ptr<Query> inner = rewritten.value() == nullptr ? query_ : rewritten.value();

    if (boost_ == 1.0F) {
        return inner;
    }

    // Fold nested boosts into one factor.
    if (const auto *nested = dynamic_cast<const BoostQuery *>(inner.get()); nested != nullptr) {
        auto folded = create(nested->query_, boost_ * nested->boost_);
        if (!folded.has_value()) {
            return std::unexpected(std::move(folded.error()));
        }
        return std::shared_ptr<Query>(std::make_shared<BoostQuery>(std::move(folded.value())));
    }

    if (dynamic_cast<const MatchNoDocsQuery *>(inner.get()) != nullptr) {
        return inner;
    }

    // A zero boost zeroes the score, but a constant score query already scores
    // every match the same, so it only needs the boost.
    if (boost_ == 0.0F && dynamic_cast<const ConstantScoreQuery *>(inner.get()) == nullptr) {
        auto zeroed = create(std::make_shared<ConstantScoreQuery>(inner), 0.0F);
        if (!zeroed.has_value()) {
            return std::unexpected(std::move(zeroed.error()));
        }
        return std::shared_ptr<Query>(std::make_shared<BoostQuery>(std::move(zeroed.value())));
    }

    if (inner->identity().is_same(query_->identity())) {
        return std::shared_ptr<Query>{};
    }
    auto wrapped = create(inner, boost_);
    if (!wrapped.has_value()) {
        return std::unexpected(std::move(wrapped.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<BoostQuery>(std::move(wrapped.value())));
}

VoidResult BoostQuery::visit(QueryVisitor &visitor) const { return query_->visit(visitor); }

Result<std::string> BoostQuery::to_string(std::string_view field) const {
    auto inner = query_->to_string(field);
    if (!inner.has_value()) {
        return std::unexpected(std::move(inner.error()));
    }
    return std::format("({})^{:.1f}", inner.value(), boost_);
}

bool BoostQuery::equals(const Query &other) const {
    const auto *boost_query = dynamic_cast<const BoostQuery *>(&other);
    if (boost_query == nullptr) {
        return false;
    }
    return BitUtil::float_to_int_bits(boost_) == BitUtil::float_to_int_bits(boost_query->boost_) &&
           query_->equals(*boost_query->query_);
}

size_t BoostQuery::hash() const {
    return query_->hash() * 31 + static_cast<size_t>(static_cast<uint32_t>(BitUtil::float_to_int_bits(boost_)));
}

} // namespace lucene
