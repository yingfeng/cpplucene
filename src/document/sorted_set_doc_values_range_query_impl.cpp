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

module lucene_core:sorted_set_doc_values_range_query.impl;

import :sorted_set_doc_values_range_query;

import std;

import :bytes_ref;
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
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :sorted_set_doc_values;
import :two_phase_iterator;
import :types;
import :weight;

namespace lucene {

namespace {

/// Matches the documents of a sorted set doc values field that have at least one
/// ordinal in `[min_ord, max_ord]`.
class SortedSetRangeMatch final : public TwoPhaseIterator {
public:
    SortedSetRangeMatch(std::unique_ptr<SortedSetDocValues> values, int64_t min_ord, int64_t max_ord)
            : values_(std::move(values)), min_ord_(min_ord), max_ord_(max_ord) {}

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
            auto ord = values_->next_ord();
            if (!ord.has_value()) {
                return std::unexpected(std::move(ord.error()));
            }
            if (ord.value() < min_ord_) {
                continue;
            }
            // The ordinals of a document are sorted, so the first ordinal that is
            // not below the lower bound decides the answer.
            return ord.value() <= max_ord_;
        }
        return false;
    }

    [[nodiscard]] float match_cost() const override { return 2.0F; }

private:
    std::unique_ptr<SortedSetDocValues> values_;
    int64_t min_ord_;
    int64_t max_ord_;
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

/// The ordinals the query selects: `[min_ord, max_ord]`, both inclusive.
struct OrdRange {
    int64_t min_ord{0};
    int64_t max_ord{0};
    bool empty{false};
};

/// Resolves the ordinal bounds of the query against the dictionary of `values`.
Result<OrdRange> resolve_ord_range(const SortedSetDocValuesRangeQuery &query, SortedSetDocValues &values) {
    OrdRange range;
    if (query.lower_value().has_value()) {
        auto ord = values.lookup_term(query.lower_value().value().as_slice());
        if (!ord.has_value()) {
            return std::unexpected(std::move(ord.error()));
        }
        if (ord.value() < 0) {
            // Not present: the insertion point is the first ordinal that is
            // greater than the bound, which is where `-ord - 1` points.
            range.min_ord = -1 - ord.value();
        } else if (query.lower_inclusive()) {
            range.min_ord = ord.value();
        } else {
            range.min_ord = ord.value() + 1;
        }
    }
    if (query.upper_value().has_value()) {
        auto ord = values.lookup_term(query.upper_value().value().as_slice());
        if (!ord.has_value()) {
            return std::unexpected(std::move(ord.error()));
        }
        if (ord.value() < 0) {
            // Not present: `-ord - 2` is the last ordinal below the bound.
            range.max_ord = -2 - ord.value();
        } else if (query.upper_inclusive()) {
            range.max_ord = ord.value();
        } else {
            range.max_ord = ord.value() - 1;
        }
    } else {
        auto count = values.get_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        range.max_ord = count.value() - 1;
    }
    range.empty = range.min_ord > range.max_ord;
    return range;
}

/// The weight of a [`SortedSetDocValuesRangeQuery`].
class RangeQueryWeight final : public Weight {
public:
    RangeQueryWeight(const SortedSetDocValuesRangeQuery &query, ScoreMode score_mode, float boost)
            : query_(query), score_mode_(score_mode), boost_(boost) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher & /*searcher*/) const override {
        auto values = context.reader()->get_sorted_set_doc_values(query_.field());
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() == nullptr) {
            return Explanation::no_match_no_details("no doc values for field " + query_.field());
        }
        auto range = resolve_ord_range(query_, *values.value());
        if (!range.has_value()) {
            return std::unexpected(std::move(range.error()));
        }
        if (range.value().empty) {
            return Explanation::no_match_no_details("no term of the dictionary is in the range");
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
            auto ord = values.value()->next_ord();
            if (!ord.has_value()) {
                return std::unexpected(std::move(ord.error()));
            }
            if (ord.value() >= range.value().min_ord && ord.value() <= range.value().max_ord) {
                return Explanation::match_no_details(boost_,
                        "ord " + std::to_string(ord.value()) + " is in [" +
                                std::to_string(range.value().min_ord) + " TO " +
                                std::to_string(range.value().max_ord) + "]");
            }
        }
        return Explanation::no_match_no_details("no ordinal of this document is in the range");
    }

    [[nodiscard]] const Query &get_query() const override { return query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher & /*searcher*/) override {
        auto field_infos = context.reader()->field_infos();
        if (!field_infos.has_value()) {
            return std::unexpected(std::move(field_infos.error()));
        }
        if (field_infos.value()->by_name(query_.field()) == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }

        auto values = context.reader()->get_sorted_set_doc_values(query_.field());
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }

        auto range = resolve_ord_range(query_, *values.value());
        if (!range.has_value()) {
            return std::unexpected(std::move(range.error()));
        }
        // No term of the dictionary is in the range.
        if (range.value().empty) {
            std::unique_ptr<DocIdSetIterator> empty(new EmptyDISI());
            std::unique_ptr<Scorer> scorer = ConstantScoreScorer::from_disi(boost_, score_mode_, std::move(empty));
            const int64_t cost = scorer->cost().value_or(0);
            return std::unique_ptr<ScorerSupplier>(new OneShotSupplier(std::move(scorer), cost));
        }

        std::shared_ptr<DocValuesSkipper> skipper;
        auto maybe_skipper = context.reader()->get_doc_values_skipper(query_.field());
        if (!maybe_skipper.has_value()) {
            return std::unexpected(std::move(maybe_skipper.error()));
        }
        if (maybe_skipper.value() != nullptr) {
            // The whole field is out of range.
            if (maybe_skipper.value()->global_min_value() > range.value().max_ord ||
                maybe_skipper.value()->global_max_value() < range.value().min_ord) {
                std::unique_ptr<DocIdSetIterator> empty(new EmptyDISI());
                std::unique_ptr<Scorer> scorer = ConstantScoreScorer::from_disi(boost_, score_mode_, std::move(empty));
                const int64_t cost = scorer->cost().value_or(0);
                return std::unique_ptr<ScorerSupplier>(new OneShotSupplier(std::move(scorer), cost));
            }
            auto max_doc = context.reader()->max_doc();
            if (!max_doc.has_value()) {
                return std::unexpected(std::move(max_doc.error()));
            }
            if (maybe_skipper.value()->global_doc_count() == max_doc.value() &&
                maybe_skipper.value()->global_min_value() >= range.value().min_ord &&
                maybe_skipper.value()->global_max_value() <= range.value().max_ord) {
                // Every document has a value in range.
                std::unique_ptr<DocIdSetIterator> all(new AllDISI(max_doc.value()));
                std::unique_ptr<Scorer> scorer = ConstantScoreScorer::from_disi(boost_, score_mode_, std::move(all));
                const int64_t cost = scorer->cost().value_or(max_doc.value());
                return std::unique_ptr<ScorerSupplier>(new OneShotSupplier(std::move(scorer), cost));
            }
            skipper = std::shared_ptr<DocValuesSkipper>(std::move(maybe_skipper.value()));
        }

        std::unique_ptr<TwoPhaseIterator> iterator(
                new SortedSetRangeMatch(std::move(values.value()), range.value().min_ord, range.value().max_ord));

        std::unique_ptr<Scorer> scorer;
        if (skipper != nullptr) {
            // The skipper skips the intervals whose values are outside the range.
            std::unique_ptr<TwoPhaseIterator> skipped(new DocValuesRangeIterator(std::move(iterator), skipper,
                    range.value().min_ord, range.value().max_ord, false));
            scorer = ConstantScoreScorer::from_tpi(boost_, score_mode_, std::move(skipped));
        } else {
            scorer = ConstantScoreScorer::from_tpi(boost_, score_mode_, std::move(iterator));
        }
        const int64_t cost = scorer->cost().value_or(0);
        return std::unique_ptr<ScorerSupplier>(new OneShotSupplier(std::move(scorer), cost));
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    const SortedSetDocValuesRangeQuery &query_;
    ScoreMode score_mode_;
    float boost_;
};

} // namespace

SortedSetDocValuesRangeQuery::SortedSetDocValuesRangeQuery(std::string field, std::optional<BytesRef> lower_value,
                                                          std::optional<BytesRef> upper_value, bool lower_inclusive,
                                                          bool upper_inclusive)
        : field_(std::move(field)),
          lower_value_(std::move(lower_value)),
          upper_value_(std::move(upper_value)),
          // An open endpoint cannot be exclusive.
          lower_inclusive_(lower_inclusive && lower_value_.has_value()),
          upper_inclusive_(upper_inclusive && upper_value_.has_value()) {}

Result<SortedSetDocValuesRangeQuery>
SortedSetDocValuesRangeQuery::create(std::string field, std::optional<BytesRef> lower_value,
                                     std::optional<BytesRef> upper_value, bool lower_inclusive,
                                     bool upper_inclusive) {
    return SortedSetDocValuesRangeQuery(std::move(field), std::move(lower_value), std::move(upper_value),
                                        lower_inclusive, upper_inclusive);
}

Result<std::unique_ptr<Weight>>
SortedSetDocValuesRangeQuery::create_weight(const IndexSearcher & /*searcher*/, ScoreMode score_mode,
                                            float boost) const {
    std::unique_ptr<Weight> weight(new RangeQueryWeight(*this, score_mode, boost));
    return weight;
}

Result<std::shared_ptr<Query>> SortedSetDocValuesRangeQuery::rewrite(const IndexSearcher & /*searcher*/) const {
    if (!lower_value_.has_value() && !upper_value_.has_value()) {
        return std::shared_ptr<Query>(std::make_shared<FieldExistsQuery>(field_));
    }
    return std::shared_ptr<Query>{};
}

VoidResult SortedSetDocValuesRangeQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        return visitor.visit_leaf(*this);
    }
    return {};
}

Result<std::string> SortedSetDocValuesRangeQuery::to_string(std::string_view field) const {
    // The reference implementation prints the endpoints through the byte display
    // of `BytesRef` (hex); the port prints UTF-8 text like `TermRangeQuery`, and
    // falls back to the byte display when the endpoint is not valid UTF-8.
    const auto endpoint = [](const std::optional<BytesRef> &value) -> std::string {
        if (!value.has_value()) {
            return "*";
        }
        auto text = value.value().utf8_to_string();
        return text.has_value() ? text.value() : value.value().to_display_string();
    };
    std::string result;
    if (field != field_) {
        result += field_ + ":";
    }
    result += lower_inclusive_ ? "[" : "{";
    result += endpoint(lower_value_);
    result += " TO ";
    result += endpoint(upper_value_);
    result += upper_inclusive_ ? "]" : "}";
    return result;
}

bool SortedSetDocValuesRangeQuery::equals(const Query &other) const {
    const auto *query = dynamic_cast<const SortedSetDocValuesRangeQuery *>(&other);
    if (query == nullptr) {
        return false;
    }
    return field_ == query->field_ && lower_value_ == query->lower_value_ && upper_value_ == query->upper_value_ &&
           lower_inclusive_ == query->lower_inclusive_ && upper_inclusive_ == query->upper_inclusive_;
}

size_t SortedSetDocValuesRangeQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    if (lower_value_.has_value()) {
        result = result * 31 + static_cast<size_t>(lower_value_.value().hash_code());
    }
    if (upper_value_.has_value()) {
        result = result * 31 + static_cast<size_t>(upper_value_.value().hash_code());
    }
    result = result * 31 + static_cast<size_t>(lower_inclusive_);
    result = result * 31 + static_cast<size_t>(upper_inclusive_);
    return result;
}

} // namespace lucene
