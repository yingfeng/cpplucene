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

module lucene_core:match_no_docs_query.impl;

import :match_no_docs_query;

import :error;
import :explanation;
import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :scorer_supplier;
import :types;
import :weight;

import std;

namespace lucene {

namespace {

/// The weight of a [`MatchNoDocsQuery`]: it never has a scorer.
class MatchNoDocsWeight final : public Weight {
public:
    MatchNoDocsWeight(const MatchNoDocsQuery &query, ScoreMode score_mode) : query_(query), score_mode_(score_mode) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher & /*searcher*/) const override {
        (void)context;
        (void)doc;
        return Explanation::no_match_no_details("MatchNoDocsQuery");
    }

    [[nodiscard]] const Query &get_query() const override { return query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext & /*context*/,
                                                                         const IndexSearcher & /*searcher*/) override {
        return std::unique_ptr<ScorerSupplier>{};
    }

    [[nodiscard]] Result<int32_t> count(const LeafReaderContext & /*context*/,
                                        const IndexSearcher & /*searcher*/) override {
        return 0;
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    const MatchNoDocsQuery &query_;
    [[maybe_unused]] ScoreMode score_mode_;
};

} // namespace

MatchNoDocsQuery::MatchNoDocsQuery(std::string reason) : reason_(std::move(reason)) {}

std::shared_ptr<MatchNoDocsQuery> MatchNoDocsQuery::create(std::string reason) {
    return std::make_shared<MatchNoDocsQuery>(std::move(reason));
}

Result<std::unique_ptr<Weight>> MatchNoDocsQuery::create_weight(const IndexSearcher & /*searcher*/,
                                                                ScoreMode score_mode, float /*boost*/) const {
    return std::unique_ptr<Weight>(new MatchNoDocsWeight(*this, score_mode));
}

VoidResult MatchNoDocsQuery::visit(QueryVisitor & /*visitor*/) const { return {}; }

Result<std::string> MatchNoDocsQuery::to_string(std::string_view /*field*/) const {
    return "MatchNoDocsQuery(\"" + reason_ + "\")";
}

bool MatchNoDocsQuery::equals(const Query &other) const {
    // The reference implementation compares the reason as well; the port matches on the type only,
    // because two "match nothing" queries are interchangeable for a query cache.
    return dynamic_cast<const MatchNoDocsQuery *>(&other) != nullptr;
}

size_t MatchNoDocsQuery::hash() const { return std::hash<std::string_view>{}("MatchNoDocsQuery"); }

} // namespace lucene
