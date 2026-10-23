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

module lucene_core:sorted_numeric_doc_values_set_query.impl;

import :sorted_numeric_doc_values_set_query;

import std;

import :constant_score_scorer;
import :doc_id_set_iterator;
import :doc_values_long_hash_set;
import :error;
import :explanation;
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
/// is in the set.
class SingletonNumericMatch final : public TwoPhaseIterator {
public:
    SingletonNumericMatch(std::unique_ptr<NumericDocValues> values, std::shared_ptr<const DocValuesLongHashSet> numbers)
            : values_(std::move(values)), numbers_(std::move(numbers)) {}

    [[nodiscard]] int32_t approx_doc_id() const override { return values_->doc_id(); }
    [[nodiscard]] Result<int32_t> approx_next_doc() override { return values_->next_doc(); }
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override { return values_->advance(target); }
    [[nodiscard]] Result<int64_t> approx_cost() const override { return values_->cost(); }

    [[nodiscard]] Result<bool> matches() override {
        auto value = values_->long_value();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        // The minimum and maximum values bound the check, which keeps the hash
        // lookup off the hot path for most documents.
        return value.value() >= numbers_->min_value() && value.value() <= numbers_->max_value() &&
               numbers_->contains(value.value());
    }

    [[nodiscard]] float match_cost() const override { return 5.0F; }

private:
    std::unique_ptr<NumericDocValues> values_;
    std::shared_ptr<const DocValuesLongHashSet> numbers_;
};

/// Matches the documents of a multi-valued sorted numeric doc values field that
/// have at least one value in the set.
class SortedNumericMatch final : public TwoPhaseIterator {
public:
    SortedNumericMatch(std::unique_ptr<SortedNumericDocValues> values,
                       std::shared_ptr<const DocValuesLongHashSet> numbers)
            : values_(std::move(values)), numbers_(std::move(numbers)) {}

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
            if (value.value() < numbers_->min_value()) {
                continue;
            }
            if (value.value() > numbers_->max_value()) {
                // The values are sorted, so the rest of them are out of range
                // too.
                return false;
            }
            if (numbers_->contains(value.value())) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] float match_cost() const override { return 5.0F; }

private:
    std::unique_ptr<SortedNumericDocValues> values_;
    std::shared_ptr<const DocValuesLongHashSet> numbers_;
};

/// A supplier that hands out one scorer; the supplier is used at most once.
class OneShotSupplier final : public ScorerSupplier {
public:
    explicit OneShotSupplier(std::unique_ptr<Scorer> scorer, int64_t cost)
            : scorer_(std::move(scorer)), cost_(cost) {}

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

/// The weight of a [`SortedNumericDocValuesSetQuery`].
class SetQueryWeight final : public Weight {
public:
    SetQueryWeight(const Query &parent, std::string field, std::shared_ptr<const DocValuesLongHashSet> numbers,
                   ScoreMode score_mode, float boost)
            : parent_(parent),
              field_(std::move(field)),
              numbers_(std::move(numbers)),
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
            if (numbers_->contains(value.value())) {
                return Explanation::match_no_details(boost_, "value " + std::to_string(value.value()) + " is in the set");
            }
        }
        return Explanation::no_match_no_details("no value of this document is in the set");
    }

    [[nodiscard]] const Query &get_query() const override { return parent_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher & /*searcher*/) override {
        auto field_infos = context.reader()->field_infos();
        if (!field_infos.has_value()) {
            return std::unexpected(std::move(field_infos.error()));
        }
        if (field_infos.value()->by_name(field_) == nullptr) {
            // The field does not exist in this leaf.
            return std::unique_ptr<ScorerSupplier>{};
        }

        auto values = context.reader()->get_sorted_numeric_doc_values(field_);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }

        std::unique_ptr<TwoPhaseIterator> iterator;
        if (values.value()->is_single_valued()) {
            // A single-valued field is cheaper through the numeric view.
            auto singleton = context.reader()->get_numeric_doc_values(field_);
            if (!singleton.has_value()) {
                return std::unexpected(std::move(singleton.error()));
            }
            if (singleton.value() == nullptr) {
                return std::unique_ptr<ScorerSupplier>{};
            }
            iterator = std::make_unique<SingletonNumericMatch>(std::move(singleton.value()), numbers_);
        } else {
            iterator = std::make_unique<SortedNumericMatch>(std::move(values.value()), numbers_);
        }

        std::unique_ptr<Scorer> scorer =
                ConstantScoreScorer::from_tpi(boost_, score_mode_, std::move(iterator));
        const int64_t cost = scorer->cost().value_or(0);
        return std::unique_ptr<ScorerSupplier>(new OneShotSupplier(std::move(scorer), cost));
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override {
        // The query only reads doc values, so its matches can be cached.
        return true;
    }

private:
    const Query &parent_;
    std::string field_;
    std::shared_ptr<const DocValuesLongHashSet> numbers_;
    ScoreMode score_mode_;
    float boost_;
};

} // namespace

SortedNumericDocValuesSetQuery::SortedNumericDocValuesSetQuery(std::string field,
                                                             std::shared_ptr<const DocValuesLongHashSet> numbers)
        : field_(std::move(field)), numbers_(std::move(numbers)) {}

Result<SortedNumericDocValuesSetQuery>
SortedNumericDocValuesSetQuery::create(std::string field, std::vector<int64_t> numbers) {
    std::sort(numbers.begin(), numbers.end());
    auto set = DocValuesLongHashSet::create(std::move(numbers));
    if (!set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }
    return SortedNumericDocValuesSetQuery(std::move(field),
                                          std::make_shared<const DocValuesLongHashSet>(std::move(set.value())));
}

Result<std::unique_ptr<Weight>> SortedNumericDocValuesSetQuery::create_weight(const IndexSearcher & /*searcher*/,
                                                                             ScoreMode score_mode,
                                                                             float boost) const {
    std::unique_ptr<Weight> weight(new SetQueryWeight(*this, field_, numbers_, score_mode, boost));
    return weight;
}

Result<std::shared_ptr<Query>> SortedNumericDocValuesSetQuery::rewrite(const IndexSearcher & /*searcher*/) const {
    if (numbers_->size() == 0) {
        return std::shared_ptr<Query>(std::make_shared<MatchNoDocsQuery>());
    }
    return std::shared_ptr<Query>{};
}

VoidResult SortedNumericDocValuesSetQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        return visitor.visit_leaf(*this);
    }
    return {};
}

Result<std::string> SortedNumericDocValuesSetQuery::to_string(std::string_view /*field*/) const {
    return field_ + ": " + numbers_->to_string();
}

bool SortedNumericDocValuesSetQuery::equals(const Query &other) const {
    const auto *query = dynamic_cast<const SortedNumericDocValuesSetQuery *>(&other);
    if (query == nullptr) {
        return false;
    }
    return field_ == query->field_ && numbers_->to_string() == query->numbers_->to_string();
}

size_t SortedNumericDocValuesSetQuery::hash() const {
    return std::hash<std::string>{}(field_) * 31 + std::hash<std::string>{}(numbers_->to_string());
}

} // namespace lucene
