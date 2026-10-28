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

module lucene_core:doc_values_rewrite_method.impl;

import :doc_values_rewrite_method;

import std;

import :constant_score_scorer;
import :doc_id_set_iterator;
import :doc_values_range_iterator;
import :doc_values_skipper;
import :error;
import :explanation;
import :fixed_bit_set;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :multi_term_query;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :singleton_sorted_set_doc_values;
import :sorted_doc_values;
import :sorted_set_doc_values;
import :sorted_set_doc_values_terms_enum;
import :terms;
import :terms_enum;
import :two_phase_iterator;
import :types;
import :weight;

namespace lucene {

namespace {

/// The reference implementation's `match_cost` of the term set approximations.
constexpr float kTermSetMatchCost = 3.0F;

/// [`Terms`] over a field's `SortedSetDocValues`: its dictionary is the term
/// dictionary.
///
/// Port of `DocValuesTerms` (`core/search/doc_values_rewrite_method.rs`).
class DocValuesTerms final : public Terms {
public:
    explicit DocValuesTerms(std::shared_ptr<SortedSetDocValues> values) : values_(std::move(values)) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(new SortedSetDocValuesTermsEnum(values_));
    }

    /// The reference implementation reports "unknown" and fails the statistics.
    [[nodiscard]] Result<int64_t> size() const override { return -1; }

    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override {
        return err(LuceneError::unsupported_operation("doc values have no term frequencies"));
    }
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override {
        return err(LuceneError::unsupported_operation("doc values have no document frequencies"));
    }
    [[nodiscard]] Result<int32_t> doc_count() const override {
        return err(LuceneError::unsupported_operation("doc values have no document count"));
    }

    [[nodiscard]] bool has_freqs() const override { return false; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }

private:
    std::shared_ptr<SortedSetDocValues> values_;
};

/// Confirms a candidate document when one of its ordinals is in the term set.
///
/// Port of `SortedSetTermSetTwoPhaseIterator`
/// (`core/search/doc_values_rewrite_method.rs`).
class SortedSetTermSetTwoPhaseIterator final : public TwoPhaseIterator {
public:
    SortedSetTermSetTwoPhaseIterator(std::shared_ptr<SortedSetDocValues> values, FixedBitSet term_set, int64_t max_ord)
            : values_(std::move(values)), term_set_(std::move(term_set)), max_ord_(max_ord) {}

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
            if (ord.value() > max_ord_) {
                // The ordinals of a document are sorted, so nothing can match.
                return false;
            }
            if (term_set_.get_bit(static_cast<size_t>(ord.value()))) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] float match_cost() const override { return kTermSetMatchCost; }

private:
    std::shared_ptr<SortedSetDocValues> values_;
    FixedBitSet term_set_;
    int64_t max_ord_;
};

/// Hands out the constant score scorer over the doc values of one leaf.
/// A [`TwoPhaseIterator`] over a single-valued cursor: a document matches when its
/// ordinal is one of the terms the query selected.
///
/// Port of `SingletonTermSetTwoPhaseIterator`
/// (`core/search/doc_values_rewrite_method.rs`).
///
/// **Deviation:** the reference implementation wraps a `SortedDocValues` in a
/// `TwoPhaseIterator` whose approximation is that same cursor; here the
/// [`TwoPhaseIterator`] already carries the approximation.
class SingletonTermSetTwoPhaseIterator final : public TwoPhaseIterator {
public:
    SingletonTermSetTwoPhaseIterator(std::unique_ptr<SortedDocValues> values, FixedBitSet term_set)
            : values_(std::move(values)), term_set_(std::move(term_set)) {}

    [[nodiscard]] int32_t approx_doc_id() const override { return values_->doc_id(); }
    [[nodiscard]] Result<int32_t> approx_next_doc() override { return values_->next_doc(); }
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override { return values_->advance(target); }
    [[nodiscard]] Result<int64_t> approx_cost() const override { return values_->cost(); }

    [[nodiscard]] Result<bool> matches() override {
        auto ord = values_->ord_value();
        if (!ord.has_value()) {
            return std::unexpected(std::move(ord.error()));
        }
        return ord.value() >= 0 && term_set_.get_bit(static_cast<size_t>(ord.value()));
    }

    /// One ordinal to look up per document.
    [[nodiscard]] float match_cost() const override { return 1.0F; }

private:
    std::unique_ptr<SortedDocValues> values_;
    FixedBitSet term_set_;
};

class DocValuesScorerSupplier final : public ScorerSupplier {
public:
    DocValuesScorerSupplier(std::shared_ptr<const MultiTermQuery> query, std::string field, int64_t cost, float score,
                            ScoreMode score_mode)
            : query_(std::move(query)), field_(std::move(field)), cost_(cost), score_(score),
              score_mode_(score_mode) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext &context,
                                                      const IndexSearcher & /*searcher*/) override {
        if (context.reader() == nullptr) {
            return empty_scorer();
        }
        auto values = context.reader()->get_sorted_set_doc_values(field_);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() == nullptr) {
            return empty_scorer();
        }
        std::shared_ptr<SortedSetDocValues> shared(std::move(values.value()));
        DocValuesTerms terms(shared);
        auto iterator = query_->terms_enum_for(terms);
        if (!iterator.has_value()) {
            return std::unexpected(std::move(iterator.error()));
        }
        TermsEnum &terms_enum = *iterator.value();
        auto first = terms_enum.next();
        if (!first.has_value()) {
            return std::unexpected(std::move(first.error()));
        }
        if (first.value() == nullptr) {
            return empty_scorer();
        }

        auto value_count = shared->get_value_count();
        if (!value_count.has_value()) {
            return std::unexpected(std::move(value_count.error()));
        }
        // A bit per ordinal of the field's dictionary, marking the terms the
        // query selected and the field actually has.
        FixedBitSet term_set(static_cast<size_t>(std::max<int64_t>(value_count.value(), 0)));
        int64_t min_ord = -1;
        int64_t max_ord = -1;
        while (true) {
            auto ord = terms_enum.ord();
            if (!ord.has_value()) {
                return std::unexpected(std::move(ord.error()));
            }
            if (ord.value() < 0) {
                return err(LuceneError::illegal_state("a doc values term enum reported a negative ord"));
            }
            term_set.set(static_cast<size_t>(ord.value()));
            if (min_ord < 0) {
                min_ord = ord.value();
            }
            max_ord = ord.value();
            auto next = terms_enum.next();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            if (next.value() == nullptr) {
                break;
            }
        }

        // The skip index of the field, when it has one, prunes whole blocks of
        // documents whose ordinals cannot intersect the terms the query selected.
        auto skipper = context.reader()->get_doc_values_skipper(field_);
        if (!skipper.has_value()) {
            return std::unexpected(std::move(skipper.error()));
        }
        std::shared_ptr<DocValuesSkipper> shared_skipper(std::move(skipper.value()));
        if (shared_skipper != nullptr &&
            (min_ord > shared_skipper->global_max_value() || max_ord < shared_skipper->global_min_value())) {
            // No document can hold one of the selected ordinals at all.
            return empty_scorer();
        }

        std::unique_ptr<TwoPhaseIterator> two_phase;
        if (auto *singleton = dynamic_cast<SingletonSortedSetDocValues *>(shared.get()); singleton != nullptr) {
            // The field has at most one value per document, so the cursor itself is
            // the approximation and only the ordinal has to be looked up.
            two_phase = std::make_unique<SingletonTermSetTwoPhaseIterator>(singleton->release(), std::move(term_set));
        } else {
            two_phase = std::make_unique<SortedSetTermSetTwoPhaseIterator>(std::move(shared), std::move(term_set),
                                                                          max_ord);
        }
        if (shared_skipper != nullptr) {
            std::unique_ptr<TwoPhaseIterator> pruned(new DocValuesRangeIterator(
                    std::move(two_phase), std::move(shared_skipper), min_ord, max_ord, /*has_gaps=*/true));
            return std::unique_ptr<Scorer>(ConstantScoreScorer::from_tpi(score_, score_mode_, std::move(pruned)));
        }
        return std::unique_ptr<Scorer>(ConstantScoreScorer::from_tpi(score_, score_mode_, std::move(two_phase)));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return cost_;
    }

private:
    [[nodiscard]] std::unique_ptr<Scorer> empty_scorer() const {
        return ConstantScoreScorer::from_disi(score_, score_mode_, std::make_unique<EmptyDISI>());
    }

    std::shared_ptr<const MultiTermQuery> query_;
    std::string field_;
    int64_t cost_;
    float score_;
    ScoreMode score_mode_;
};

/// The weight of a [`MultiTermQueryDocValuesWrapper`].
class MultiTermQueryDocValuesWeight final : public Weight {
public:
    MultiTermQueryDocValuesWeight(const Query &parent_query, std::shared_ptr<const MultiTermQuery> query, float boost,
                                  ScoreMode score_mode)
            : parent_query_(&parent_query), query_(std::move(query)), boost_(boost), score_mode_(score_mode) {}

    /// The doc values of a field never change inside a leaf, so the weight can be
    /// cached when the field has them.
    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override {
        (void)context;
        return true;
    }

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override {
        auto scorer = const_cast<MultiTermQueryDocValuesWeight *>(this)->scorer(context, searcher);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        auto text = parent_query_->to_string("");
        const std::string description = text.has_value() ? text.value() : std::string("MultiTermQueryDocValues");
        if (scorer.value() == nullptr) {
            return Explanation::no_match(std::format("no matching docs in {}, doc={}", description, doc), {});
        }
        if (auto advanced = scorer.value()->advance(doc); !advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (scorer.value()->doc_id() != doc) {
            return Explanation::no_match(std::format("no matching docs in {}, doc={}", description, doc), {});
        }
        auto score = scorer.value()->score();
        if (!score.has_value()) {
            return std::unexpected(std::move(score.error()));
        }
        return Explanation::match_(score.value(), description, {});
    }

    [[nodiscard]] const Query &get_query() const override { return *parent_query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher & /*searcher*/) override {
        if (context.reader() == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        const std::string field(query_->field());
        auto values = context.reader()->get_sorted_set_doc_values(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        auto value_count = values.value()->get_value_count();
        if (!value_count.has_value()) {
            return std::unexpected(std::move(value_count.error()));
        }
        if (value_count.value() == 0) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        auto cost = values.value()->cost();
        if (!cost.has_value()) {
            return std::unexpected(std::move(cost.error()));
        }
        return std::unique_ptr<ScorerSupplier>(
                new DocValuesScorerSupplier(query_, field, cost.value(), boost_, score_mode_));
    }

private:
    const Query *parent_query_;
    std::shared_ptr<const MultiTermQuery> query_;
    float boost_;
    ScoreMode score_mode_;
};

} // namespace

MultiTermQueryDocValuesWrapper::MultiTermQueryDocValuesWrapper(std::shared_ptr<const MultiTermQuery> query)
        : query_(std::move(query)) {}

Result<std::shared_ptr<MultiTermQueryDocValuesWrapper>>
MultiTermQueryDocValuesWrapper::create(std::shared_ptr<const MultiTermQuery> query) {
    if (query == nullptr) {
        return err(LuceneError::illegal_argument("the doc values wrapper needs a multi term query"));
    }
    std::shared_ptr<MultiTermQueryDocValuesWrapper> wrapper(
            new MultiTermQueryDocValuesWrapper(std::move(query)));
    return wrapper;
}

std::string_view MultiTermQueryDocValuesWrapper::field() const { return query_->field(); }

Result<std::unique_ptr<Weight>> MultiTermQueryDocValuesWrapper::create_weight(const IndexSearcher &searcher,
                                                                            ScoreMode score_mode,
                                                                            float boost) const {
    (void)searcher;
    std::unique_ptr<Weight> weight(new MultiTermQueryDocValuesWeight(*this, query_, boost, score_mode));
    return weight;
}

VoidResult MultiTermQueryDocValuesWrapper::visit(QueryVisitor &visitor) const {
    if (!visitor.accept_field(query_->field())) {
        return {};
    }
    // **Deviation:** the reference implementation visits through a sub visitor for
    // the `FILTER` occurrence; the port has no `get_sub_visitor` yet.
    return query_->visit(visitor);
}

Result<std::string> MultiTermQueryDocValuesWrapper::to_string(std::string_view field) const {
    return query_->to_string(field);
}

bool MultiTermQueryDocValuesWrapper::equals(const Query &other) const {
    const auto *wrapper = dynamic_cast<const MultiTermQueryDocValuesWrapper *>(&other);
    if (wrapper == nullptr) {
        return false;
    }
    return query_->equals(*wrapper->query_);
}

size_t MultiTermQueryDocValuesWrapper::hash() const { return query_->hash() * 31 + 0x7A5D2C19; }

} // namespace lucene
