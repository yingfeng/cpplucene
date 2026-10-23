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

module lucene_core:index_or_doc_values_query.impl;

import :index_or_doc_values_query;

import std;

import :error;
import :explanation;
import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :types;
import :weight;

namespace lucene {

IndexOrDocValuesQuery::IndexOrDocValuesQuery(std::shared_ptr<Query> index_query, std::shared_ptr<Query> dv_query)
        : index_query_(std::move(index_query)), dv_query_(std::move(dv_query)) {}

Result<IndexOrDocValuesQuery> IndexOrDocValuesQuery::create(std::shared_ptr<Query> index_query,
                                                           std::shared_ptr<Query> dv_query) {
    if (index_query == nullptr || dv_query == nullptr) {
        return err(LuceneError::illegal_argument("both wrapped queries must not be null"));
    }
    return IndexOrDocValuesQuery(std::move(index_query), std::move(dv_query));
}

Result<std::unique_ptr<Weight>> IndexOrDocValuesQuery::create_weight(const IndexSearcher &searcher,
                                                                    ScoreMode score_mode, float boost) const {
    auto weight = IndexOrDocValuesWeight::create(*this, searcher, score_mode, boost);
    if (!weight.has_value()) {
        return std::unexpected(std::move(weight.error()));
    }
    std::unique_ptr<Weight> result = std::move(weight.value());
    return result;
}

VoidResult IndexOrDocValuesQuery::visit(QueryVisitor &visitor) const { return index_query_->visit(visitor); }

Result<std::string> IndexOrDocValuesQuery::to_string(std::string_view field) const {
    auto index = index_query_->to_string(field);
    if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
    }
    auto dv = dv_query_->to_string(field);
    if (!dv.has_value()) {
        return std::unexpected(std::move(dv.error()));
    }
    return std::format("IndexOrDocValuesQuery(indexQuery={}, dvQuery={})", index.value(), dv.value());
}

bool IndexOrDocValuesQuery::equals(const Query &other) const {
    const auto *pair = dynamic_cast<const IndexOrDocValuesQuery *>(&other);
    if (pair == nullptr) {
        return false;
    }
    return index_query_->equals(*pair->index_query_) && dv_query_->equals(*pair->dv_query_);
}

size_t IndexOrDocValuesQuery::hash() const { return index_query_->hash() * 31 + dv_query_->hash(); }

IndexOrDocValuesWeight::IndexOrDocValuesWeight(const IndexOrDocValuesQuery &query,
                                               std::unique_ptr<Weight> index_weight,
                                               std::unique_ptr<Weight> dv_weight)
        : query_(query), index_weight_(std::move(index_weight)), dv_weight_(std::move(dv_weight)) {}

Result<std::unique_ptr<IndexOrDocValuesWeight>> IndexOrDocValuesWeight::create(const IndexOrDocValuesQuery &query,
                                                                              const IndexSearcher &searcher,
                                                                              ScoreMode score_mode, float boost) {
    auto index_weight = query.index_query()->create_weight(searcher, score_mode, boost);
    if (!index_weight.has_value()) {
        return std::unexpected(std::move(index_weight.error()));
    }
    auto dv_weight = query.dv_query()->create_weight(searcher, score_mode, boost);
    if (!dv_weight.has_value()) {
        return std::unexpected(std::move(dv_weight.error()));
    }
    return std::unique_ptr<IndexOrDocValuesWeight>(new IndexOrDocValuesWeight(
            query, std::move(index_weight.value()), std::move(dv_weight.value())));
}

Result<std::unique_ptr<ScorerSupplier>> IndexOrDocValuesWeight::scorer_supplier(const LeafReaderContext &context,
                                                                               const IndexSearcher &searcher) {
    auto dv_supplier = dv_weight_->scorer_supplier(context, searcher);
    if (!dv_supplier.has_value()) {
        return std::unexpected(std::move(dv_supplier.error()));
    }
    auto index_supplier = index_weight_->scorer_supplier(context, searcher);
    if (!index_supplier.has_value()) {
        return std::unexpected(std::move(index_supplier.error()));
    }

    if (dv_supplier.value() == nullptr) {
        return std::move(index_supplier.value());
    }
    if (index_supplier.value() == nullptr) {
        return std::move(dv_supplier.value());
    }

    auto dv_cost = dv_supplier.value()->cost(context, searcher);
    if (!dv_cost.has_value()) {
        return std::unexpected(std::move(dv_cost.error()));
    }
    auto index_cost = index_supplier.value()->cost(context, searcher);
    if (!index_cost.has_value()) {
        return std::unexpected(std::move(index_cost.error()));
    }

    // Take the cheaper of the two; a tie keeps the doc values query, which is the
    // one that is cheap to set up.
    if (dv_cost.value() <= index_cost.value()) {
        return std::move(dv_supplier.value());
    }
    return std::move(index_supplier.value());
}

Result<Explanation> IndexOrDocValuesWeight::explain(const LeafReaderContext &context, int32_t doc,
                                                    const IndexSearcher &searcher) const {
    return dv_weight_->explain(context, doc, searcher);
}

} // namespace lucene
