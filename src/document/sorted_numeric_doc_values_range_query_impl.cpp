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

module lucene_core:sorted_numeric_doc_values_range_query.impl;

import :sorted_numeric_doc_values_range_query;

import std;

import :constant_score_scorer;
import :doc_id_set_iterator;
import :doc_values_range_iterator;
import :doc_values_skipper;
import :error;
import :explanation;
import :field_exists_query;
import :field_infos;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :match_no_docs_query;
import :numeric_doc_values;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :sorted_numeric_doc_values;
import :two_phase_iterator;
import :types;
import :weight;

namespace lucene {

namespace {

/// Matches the documents of a single-valued numeric doc values field whose value
/// is in the range.
class SingletonRangeMatch final : public TwoPhaseIterator {
public:
    SingletonRangeMatch(std::unique_ptr<NumericDocValues> values, int64_t lower_value, int64_t upper_value)
            : values_(std::move(values)), lower_value_(lower_value), upper_value_(upper_value) {}

    [[nodiscard]] int32_t approx_doc_id() const override { return values_->doc_id(); }
    [[nodiscard]] Result<int32_t> approx_next_doc() override { return values_->next_doc(); }
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override { return values_->advance(target); }
    [[nodiscard]] Result<int64_t> approx_cost() const override { return values_->cost(); }

    [[nodiscard]] Result<bool> matches() override {
        auto value = values_->long_value();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        return value.value() >= lower_value_ && value.value() <= upper_value_;
    }

    [[nodiscard]] float match_cost() const override { return 2.0F; }

private:
    std::unique_ptr<NumericDocValues> values_;
    int64_t lower_value_;
    int64_t upper_value_;
};

/// Matches the documents of a multi-valued sorted numeric doc values field that
/// have at least one value in the range.
class SortedNumericRangeMatch final : public TwoPhaseIterator {
public:
    SortedNumericRangeMatch(std::unique_ptr<SortedNumericDocValues> values, int64_t lower_value, int64_t upper_value)
            : values_(std::move(values)), lower_value_(lower_value), upper_value_(upper_value) {}

    [[nodiscard]] int32_t approx_doc_id() const override { return values_->doc_id(); }
    [[nodiscard]] Result<int32_t> approx_next_doc() override { return values_->next_doc(); }
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override { return values_->advance(target); }
    [[nodiscard]] Result<int64_t> approx_cost() const override { return values_->cost(); }

    [[nodiscard]] Result<bool> matches() override {
        auto count = values_->doc_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        for (int32_t i = 0; i < count.value(); ++i) {
            auto value = values_->next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            if (value.value() < lower_value_) {
                continue;
            }
            // The values are sorted, so the first value that is not below the
            // lower bound decides the answer.
            return value.value() <= upper_value_;
        }
        return false;
    }

    [[nodiscard]] float match_cost() const override { return 2.0F; }

private:
    std::unique_ptr<SortedNumericDocValues> values_;
    int64_t lower_value_;
    int64_t upper_value_;
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

/// The weight of a [`SortedNumericDocValuesRangeQuery`].
class RangeQueryWeight final : public Weight {
public:
    RangeQueryWeight(const Query &parent, std::string field, int64_t lower_value, int64_t upper_value,
                     ScoreMode score_mode, float boost)
            : parent_(parent),
              field_(std::move(field)),
              lower_value_(lower_value),
              upper_value_(upper_value),
              score_mode_(score_mode),
              boost_(boost) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher & /*searcher*/) const override {
        auto values = context.reader()->get_sorted_numeric_doc_values(field_);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() == nullptr) {
            return Explanation::no_match_no_details("no doc values for field " + field_);
        }
        auto advanced = values.value()->advance(doc);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (advanced.value() != doc) {
            return Explanation::no_match_no_details("no doc values for this document");
        }
        auto count = values.value()->doc_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        for (int32_t i = 0; i < count.value(); ++i) {
            auto value = values.value()->next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            if (value.value() >= lower_value_ && value.value() <= upper_value_) {
                return Explanation::match_no_details(boost_,
                        "value " + std::to_string(value.value()) + " is in [" + std::to_string(lower_value_) + " TO " +
                                std::to_string(upper_value_) + "]");
            }
        }
        return Explanation::no_match_no_details("no value of this document is in the range");
    }

    [[nodiscard]] const Query &get_query() const override { return parent_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher & /*searcher*/) override {
        auto field_infos = context.reader()->field_infos();
        if (!field_infos.has_value()) {
            return std::unexpected(std::move(field_infos.error()));
        }
        if (field_infos.value()->by_name(field_) == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }

        std::shared_ptr<DocValuesSkipper> skipper;
        auto maybe_skipper = context.reader()->get_doc_values_skipper(field_);
        if (!maybe_skipper.has_value()) {
            return std::unexpected(std::move(maybe_skipper.error()));
        }
        if (maybe_skipper.value() != nullptr) {
            skipper = std::shared_ptr<DocValuesSkipper>(std::move(maybe_skipper.value()));
            // The whole field is out of range.
            if (skipper->global_min_value() > upper_value_ || skipper->global_max_value() < lower_value_) {
                return std::unique_ptr<ScorerSupplier>{};
            }
            auto max_doc = context.reader()->max_doc();
            if (!max_doc.has_value()) {
                return std::unexpected(std::move(max_doc.error()));
            }
            if (skipper->global_doc_count() == max_doc.value() && skipper->global_min_value() >= lower_value_ &&
                skipper->global_max_value() <= upper_value_) {
                // Every document has a value in range.
                std::unique_ptr<DocIdSetIterator> all(new AllDISI(max_doc.value()));
                std::unique_ptr<Scorer> scorer = ConstantScoreScorer::from_disi(boost_, score_mode_, std::move(all));
                const int64_t cost = scorer->cost().value_or(max_doc.value());
                return std::unique_ptr<ScorerSupplier>(new OneShotSupplier(std::move(scorer), cost));
            }
        }

        auto values = context.reader()->get_sorted_numeric_doc_values(field_);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }

        std::unique_ptr<TwoPhaseIterator> iterator;
        // The single-valued fast path needs the numeric view of the field.  A field
        // that is stored as sorted numeric doc values has none, so the values are
        // read through the sorted numeric cursor instead — which is what the
        // multi-valued path does anyway.
        std::unique_ptr<NumericDocValues> singleton;
        if (values.value()->is_single_valued()) {
            auto maybe_singleton = context.reader()->get_numeric_doc_values(field_);
            if (!maybe_singleton.has_value()) {
                return std::unexpected(std::move(maybe_singleton.error()));
            }
            singleton = std::move(maybe_singleton.value());
        }
        if (singleton != nullptr) {
            iterator = std::make_unique<SingletonRangeMatch>(std::move(singleton), lower_value_, upper_value_);
        } else {
            iterator = std::make_unique<SortedNumericRangeMatch>(std::move(values.value()), lower_value_, upper_value_);
        }

        std::unique_ptr<Scorer> scorer;
        if (skipper != nullptr) {
            // The skipper skips the intervals whose values are outside the range.
            std::unique_ptr<TwoPhaseIterator> skipped(
                    new DocValuesRangeIterator(std::move(iterator), skipper, lower_value_, upper_value_, false));
            scorer = ConstantScoreScorer::from_tpi(boost_, score_mode_, std::move(skipped));
        } else {
            scorer = ConstantScoreScorer::from_tpi(boost_, score_mode_, std::move(iterator));
        }
        const int64_t cost = scorer->cost().value_or(0);
        return std::unique_ptr<ScorerSupplier>(new OneShotSupplier(std::move(scorer), cost));
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override {
        return true;
    }

private:
    const Query &parent_;
    std::string field_;
    int64_t lower_value_;
    int64_t upper_value_;
    ScoreMode score_mode_;
    float boost_;
};

} // namespace

SortedNumericDocValuesRangeQuery::SortedNumericDocValuesRangeQuery(std::string field, int64_t lower_value,
                                                                 int64_t upper_value)
        : field_(std::move(field)), lower_value_(lower_value), upper_value_(upper_value) {}

Result<SortedNumericDocValuesRangeQuery>
SortedNumericDocValuesRangeQuery::create(std::string field, int64_t lower_value, int64_t upper_value) {
    return SortedNumericDocValuesRangeQuery(std::move(field), lower_value, upper_value);
}

Result<std::unique_ptr<Weight>> SortedNumericDocValuesRangeQuery::create_weight(const IndexSearcher & /*searcher*/,
                                                                              ScoreMode score_mode,
                                                                              float boost) const {
    std::unique_ptr<Weight> weight(new RangeQueryWeight(*this, field_, lower_value_, upper_value_, score_mode, boost));
    return weight;
}

Result<std::shared_ptr<Query>> SortedNumericDocValuesRangeQuery::rewrite(const IndexSearcher & /*searcher*/) const {
    if (lower_value_ == std::numeric_limits<int64_t>::min() && upper_value_ == std::numeric_limits<int64_t>::max()) {
        return std::shared_ptr<Query>(std::make_shared<FieldExistsQuery>(field_));
    }
    if (lower_value_ > upper_value_) {
        return std::shared_ptr<Query>(std::make_shared<MatchNoDocsQuery>());
    }
    return std::shared_ptr<Query>{};
}

VoidResult SortedNumericDocValuesRangeQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        return visitor.visit_leaf(*this);
    }
    return {};
}

Result<std::string> SortedNumericDocValuesRangeQuery::to_string(std::string_view field) const {
    std::string result;
    if (field != field_) {
        result += field_ + ":";
    }
    result += "[" + std::to_string(lower_value_) + " TO " + std::to_string(upper_value_) + "]";
    return result;
}

bool SortedNumericDocValuesRangeQuery::equals(const Query &other) const {
    const auto *query = dynamic_cast<const SortedNumericDocValuesRangeQuery *>(&other);
    if (query == nullptr) {
        return false;
    }
    return field_ == query->field_ && lower_value_ == query->lower_value_ && upper_value_ == query->upper_value_;
}

size_t SortedNumericDocValuesRangeQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    result = result * 31 + static_cast<size_t>(lower_value_);
    result = result * 31 + static_cast<size_t>(upper_value_);
    return result;
}

} // namespace lucene
