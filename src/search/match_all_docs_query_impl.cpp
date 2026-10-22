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

module lucene_core:match_all_docs_query.impl;

import :match_all_docs_query;

import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :types;
import :weight;

import std;

namespace lucene {

namespace {

/// Iterates every document of a leaf: `0 .. max_doc - 1`.
class AllDocsIterator final : public DocIdSetIterator {
public:
    explicit AllDocsIterator(int32_t max_doc) : max_doc_(max_doc) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        if (doc_ + 1 >= max_doc_) {
            doc_ = NO_MORE_DOCS;
            return doc_;
        }
        ++doc_;
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        if (target <= doc_) {
            return doc_;
        }
        doc_ = target >= max_doc_ ? NO_MORE_DOCS : target;
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return max_doc_; }

private:
    int32_t max_doc_;
    int32_t doc_{-1};
};

class AllDocsScorerSupplier final : public ScorerSupplier {
public:
    AllDocsScorerSupplier(int32_t max_doc, float score, ScoreMode score_mode)
        : max_doc_(max_doc), score_(score), score_mode_(score_mode) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        return std::unique_ptr<Scorer>(
                ConstantScoreScorer::from_disi(score_, score_mode_, std::make_unique<AllDocsIterator>(max_doc_)));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return static_cast<int64_t>(max_doc_);
    }

private:
    int32_t max_doc_;
    float score_;
    ScoreMode score_mode_;
};

class MatchAllWeight final : public Weight {
public:
    MatchAllWeight(const MatchAllDocsQuery &query, ScoreMode score_mode, float score)
        : query_(query), score_mode_(score_mode), score_(score) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext & /*context*/, int32_t doc,
                                              const IndexSearcher & /*searcher*/) const override {
        (void)doc;
        return Explanation::match_no_details(score_, "MatchAllDocsQuery");
    }

    [[nodiscard]] const Query &get_query() const override { return query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher & /*searcher*/) override {
        auto max_doc = context.reader()->max_doc();
        if (!max_doc.has_value()) {
            return std::unexpected(std::move(max_doc.error()));
        }
        return std::unique_ptr<ScorerSupplier>(new AllDocsScorerSupplier(max_doc.value(), score_, score_mode_));
    }

    [[nodiscard]] Result<int32_t> count(const LeafReaderContext &context,
                                        const IndexSearcher & /*searcher*/) override {
        return context.reader()->num_docs();
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    const MatchAllDocsQuery &query_;
    ScoreMode score_mode_;
    float score_;
};

} // namespace

MatchAllDocsQuery::MatchAllDocsQuery() = default;

std::shared_ptr<MatchAllDocsQuery> MatchAllDocsQuery::create() { return std::make_shared<MatchAllDocsQuery>(); }

Result<std::unique_ptr<Weight>> MatchAllDocsQuery::create_weight(const IndexSearcher & /*searcher*/,
                                                                 ScoreMode score_mode, float boost) const {
    return std::unique_ptr<Weight>(new MatchAllWeight(*this, score_mode, boost));
}

VoidResult MatchAllDocsQuery::visit(QueryVisitor &visitor) const { return visitor.visit_leaf(*this); }

Result<std::string> MatchAllDocsQuery::to_string(std::string_view /*field*/) const { return "*:*"; }

bool MatchAllDocsQuery::equals(const Query &other) const {
    return dynamic_cast<const MatchAllDocsQuery *>(&other) != nullptr;
}

size_t MatchAllDocsQuery::hash() const { return std::hash<std::string_view>{}("MatchAllDocsQuery"); }

} // namespace lucene
