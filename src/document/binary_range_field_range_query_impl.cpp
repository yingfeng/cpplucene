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

module lucene_core:binary_range_field_range_query.impl;

import :binary_range_field_range_query;

import std;

import :binary_doc_values;
import :binary_range_doc_values;
import :constant_score_scorer;
import :error;
import :explanation;
import :field_infos;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :range_field_query;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :two_phase_iterator;
import :types;
import :weight;

namespace lucene {

namespace {

/// Confirms, document by document, that the stored range satisfies the query.
class BinaryRangeMatch final : public TwoPhaseIterator {
public:
    BinaryRangeMatch(std::unique_ptr<BinaryRangeDocValues> values, std::vector<uint8_t> query_packed_value,
                     size_t num_dims, size_t num_bytes_per_dimension, QueryType query_type)
            : values_(std::move(values)),
              query_packed_value_(std::move(query_packed_value)),
              num_dims_(num_dims),
              num_bytes_per_dimension_(num_bytes_per_dimension),
              query_type_(query_type) {}

    [[nodiscard]] int32_t approx_doc_id() const override { return values_->doc_id(); }
    [[nodiscard]] Result<int32_t> approx_next_doc() override { return values_->next_doc(); }
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override { return values_->advance(target); }
    [[nodiscard]] Result<int64_t> approx_cost() const override { return values_->cost(); }

    [[nodiscard]] Result<bool> matches() override {
        return range_field_query_matches(query_type_, query_packed_value_, values_->packed_value(), num_dims_,
                                         num_bytes_per_dimension_);
    }

    [[nodiscard]] float match_cost() const override {
        return static_cast<float>(query_packed_value_.size());
    }

private:
    std::unique_ptr<BinaryRangeDocValues> values_;
    std::vector<uint8_t> query_packed_value_;
    size_t num_dims_;
    size_t num_bytes_per_dimension_;
    QueryType query_type_;
};

/// A supplier that hands out one scorer; the supplier is used at most once.
class OneShotSupplier final : public ScorerSupplier {
public:
    OneShotSupplier(std::unique_ptr<Scorer> scorer, int64_t cost) : scorer_(std::move(scorer)), cost_(cost) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        return std::move(scorer_);
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return cost_;
    }

private:
    std::unique_ptr<Scorer> scorer_;
    int64_t cost_;
};

/// The weight of a [`BinaryRangeFieldRangeQuery`].
class BinaryRangeWeight final : public Weight {
public:
    BinaryRangeWeight(const BinaryRangeFieldRangeQuery &query, ScoreMode score_mode, float boost)
        : query_(query), score_mode_(score_mode), boost_(boost) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                             const IndexSearcher & /*searcher*/) const override {
        auto scorer = scorer_for(context);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        if (scorer.value() == nullptr) {
            return Explanation::no_match_no_details("no doc values for field " + query_.field());
        }
        auto advanced = scorer.value()->advance(doc);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (advanced.value() != doc) {
            return Explanation::no_match_no_details("no doc values for this document");
        }
        auto matched = scorer.value()->matches();
        if (!matched.has_value()) {
            return std::unexpected(std::move(matched.error()));
        }
        if (!matched.value()) {
            return Explanation::no_match_no_details("the stored range does not match");
        }
        return Explanation::match_no_details(boost_, "the stored range matches");
    }

    [[nodiscard]] const Query &get_query() const override { return query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher & /*searcher*/) override {
        auto scorer = scorer_for(context);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        if (scorer.value() == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        const int64_t cost = scorer.value()->cost().value_or(0);
        std::unique_ptr<ScorerSupplier> supplier(new OneShotSupplier(std::move(scorer.value()), cost));
        return supplier;
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    /// Builds the two-phase scorer over the binary doc values of one leaf.
    [[nodiscard]] Result<std::unique_ptr<Scorer>> scorer_for(const LeafReaderContext &context) const {
        LeafReader *reader = context.reader();
        if (reader == nullptr) {
            return std::unique_ptr<Scorer>{};
        }
        auto field_infos = reader->field_infos();
        if (!field_infos.has_value()) {
            return std::unexpected(std::move(field_infos.error()));
        }
        if (field_infos.value() == nullptr || field_infos.value()->by_name(query_.field()) == nullptr) {
            return std::unique_ptr<Scorer>{};
        }
        auto values = reader->get_binary_doc_values(query_.field());
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() == nullptr) {
            return std::unique_ptr<Scorer>{};
        }
        std::unique_ptr<BinaryRangeDocValues> ranges(new BinaryRangeDocValues(
                std::move(values.value()), query_.num_dims(), query_.num_bytes_per_dimension()));
        std::unique_ptr<TwoPhaseIterator> match(new BinaryRangeMatch(std::move(ranges), query_.query_packed_value(),
                                                                    query_.num_dims(),
                                                                    query_.num_bytes_per_dimension(),
                                                                    query_.query_type()));
        return std::unique_ptr<Scorer>(ConstantScoreScorer::from_tpi(boost_, score_mode_, std::move(match)));
    }

    const BinaryRangeFieldRangeQuery &query_;
    ScoreMode score_mode_;
    float boost_;
};

} // namespace

BinaryRangeFieldRangeQuery::BinaryRangeFieldRangeQuery(std::string field, std::vector<uint8_t> query_packed_value,
                                                       size_t num_bytes_per_dimension, size_t num_dims,
                                                       QueryType query_type, std::string range_text)
        : field_(std::move(field)),
          query_packed_value_(std::move(query_packed_value)),
          num_dims_(num_dims),
          num_bytes_per_dimension_(num_bytes_per_dimension),
          query_type_(query_type),
          range_text_(std::move(range_text)) {}

Result<BinaryRangeFieldRangeQuery>
BinaryRangeFieldRangeQuery::create(std::string field, std::vector<uint8_t> query_packed_value,
                                   size_t num_bytes_per_dimension, size_t num_dims, QueryType query_type,
                                   std::string range_text) {
    if (query_type != QueryType::Intersects) {
        return err(LuceneError::unsupported_operation(
                "INTERSECTS is the only query type supported for this field type right now"));
    }
    if (num_dims == 0) {
        return err(LuceneError::illegal_argument("dimension size cannot be zero"));
    }
    if (query_packed_value.size() != 2 * num_dims * num_bytes_per_dimension) {
        return err(LuceneError::illegal_argument("the packed value does not match the given dimensions"));
    }
    return BinaryRangeFieldRangeQuery(std::move(field), std::move(query_packed_value), num_bytes_per_dimension,
                                      num_dims, query_type, std::move(range_text));
}

Result<std::unique_ptr<Weight>> BinaryRangeFieldRangeQuery::create_weight(const IndexSearcher & /*searcher*/,
                                                                        ScoreMode score_mode, float boost) const {
    return std::unique_ptr<Weight>(new BinaryRangeWeight(*this, score_mode, boost));
}

VoidResult BinaryRangeFieldRangeQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        return visitor.visit_leaf(*this);
    }
    return {};
}

Result<std::string> BinaryRangeFieldRangeQuery::to_string(std::string_view field) const {
    std::string result;
    if (field != field_) {
        result += field_ + ":";
    }
    result += range_text_;
    return result;
}

bool BinaryRangeFieldRangeQuery::equals(const Query &other) const {
    const auto *query = dynamic_cast<const BinaryRangeFieldRangeQuery *>(&other);
    if (query == nullptr) {
        return false;
    }
    return field_ == query->field_ && range_text_ == query->range_text_ &&
           query_packed_value_ == query->query_packed_value_ && num_dims_ == query->num_dims_ &&
           num_bytes_per_dimension_ == query->num_bytes_per_dimension_ && query_type_ == query->query_type_;
}

size_t BinaryRangeFieldRangeQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    result = result * 31 + std::hash<std::string>{}(range_text_);
    result = result * 31 + num_dims_;
    result = result * 31 + num_bytes_per_dimension_;
    result = result * 31 + static_cast<size_t>(query_type_);
    for (const uint8_t byte : query_packed_value_) {
        result = result * 31 + byte;
    }
    return result;
}

} // namespace lucene
