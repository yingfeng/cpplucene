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

module lucene_core:multi_term_query.impl;

import :multi_term_query;

import std;

import :boolean_clause;
import :boolean_query;
import :bytes_ref;
import :constant_score_query;
import :doc_values_rewrite_method;
import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :match_no_docs_query;
import :postings_enum;
import :query;
import :query_visitor;
import :score_mode;
import :scorable;
import :scorer;
import :scorer_supplier;
import :segment_cacheable;
import :term;
import :term_collecting_rewrite;
import :term_query;
import :term_states;
import :terms;
import :terms_enum;
import :top_terms_rewrite;
import :types;
import :weight;

namespace lucene {

namespace {

/// How many terms a constant score rewrite expands into a boolean query before
/// it switches to collecting documents instead.
///
/// `BOOLEAN_REWRITE_TERM_COUNT_THRESHOLD` of the reference implementation
/// (`core/search/abstract_multi_term_query_constant_score_wrapper.rs`).
constexpr size_t kBooleanRewriteTermCountThreshold = 16;

/// Sorts and deduplicates the terms found in different segments.
void sort_and_dedup(std::vector<Term> &terms) {
    std::ranges::sort(terms, [](const Term &left, const Term &right) { return left.compare_to(right) < 0; });
    terms.erase(std::unique(terms.begin(), terms.end(),
                            [](const Term &left, const Term &right) { return left.compare_to(right) == 0; }),
                terms.end());
}

/// Every term of `query`'s field that the query matches, sorted and
/// deduplicated.
Result<std::vector<Term>> expand_terms(const IndexSearcher &searcher, const MultiTermQuery &query) {
    std::vector<Term> terms;
    auto leaves = searcher.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    for (const LeafReaderContext &context : leaves.value()) {
        if (context.reader() == nullptr) {
            continue;
        }
        auto field_terms = context.reader()->terms(query.field());
        if (!field_terms.has_value()) {
            return std::unexpected(std::move(field_terms.error()));
        }
        if (field_terms.value() == nullptr) {
            // The leaf has no terms for this field.
            continue;
        }
        auto iterator = query.terms_enum_for(*field_terms.value());
        if (!iterator.has_value()) {
            return std::unexpected(std::move(iterator.error()));
        }
        while (true) {
            auto next = iterator.value()->next();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            if (next.value() == nullptr) {
                break;
            }
            terms.emplace_back(query.field(), *next.value());
        }
    }
    sort_and_dedup(terms);
    return terms;
}

/// Turns `terms` into a boolean query; `constant_score` wraps the result.
Result<std::shared_ptr<Query>> boolean_rewrite(const std::vector<Term> &terms, bool constant_score) {
    if (terms.empty()) {
        return std::shared_ptr<Query>(new MatchNoDocsQuery());
    }
    const size_t max_clauses = BooleanQuery::max_clause_count();
    if (terms.size() > max_clauses) {
        return err(LuceneError::too_many_clauses(std::format("maxClauseCount is set to {}", max_clauses)));
    }
    BooleanQuery::Builder builder;
    for (const Term &term : terms) {
        if (auto added = builder.add(std::make_shared<TermQuery>(term), Occur::Should); !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
    }
    auto expanded = builder.build();
    if (!expanded.has_value()) {
        return std::unexpected(std::move(expanded.error()));
    }
    std::shared_ptr<Query> query = std::make_shared<BooleanQuery>(std::move(expanded.value()));
    if (constant_score) {
        query = std::make_shared<ConstantScoreQuery>(std::move(query));
    }
    return query;
}

/// A [`DocIdSetIterator`] over ascending, deduplicated document ids.
class SortedDocIdsIterator final : public DocIdSetIterator {
public:
    explicit SortedDocIdsIterator(std::vector<int32_t> docs) : docs_(std::move(docs)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++index_;
        return set_doc();
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_ != NO_MORE_DOCS && doc_ < target) {
            if (auto next = next_doc(); !next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

private:
    [[nodiscard]] int32_t set_doc() {
        if (index_ >= static_cast<int32_t>(docs_.size())) {
            doc_ = NO_MORE_DOCS;
        } else {
            doc_ = docs_[static_cast<size_t>(index_)];
        }
        return doc_;
    }

    std::vector<int32_t> docs_;
    int32_t index_{-1};
    int32_t doc_{-1};
};

/// A supplier that hands out a scorer that was already built.
class ImmediateScorerSupplier final : public ScorerSupplier {
public:
    ImmediateScorerSupplier(std::unique_ptr<Scorer> scorer, int64_t cost)
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

/// Forwards to the supplier of a weight, keeping that weight alive.
class DelegatingScorerSupplier final : public ScorerSupplier {
public:
    DelegatingScorerSupplier(std::unique_ptr<Weight> weight, std::unique_ptr<ScorerSupplier> inner)
            : weight_(std::move(weight)), inner_(std::move(inner)) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t lead_cost, const LeafReaderContext &context,
                                                      const IndexSearcher &searcher) override {
        return inner_->get(lead_cost, context, searcher);
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext &context, const IndexSearcher &searcher) override {
        return inner_->cost(context, searcher);
    }

    [[nodiscard]] VoidResult set_top_level_scoring_clause() override {
        return inner_->set_top_level_scoring_clause();
    }

private:
    std::unique_ptr<Weight> weight_;
    std::unique_ptr<ScorerSupplier> inner_;
};

/// The weight of [`MultiTermQueryConstantScoreWrapper`].
class ConstantScoreRewritingWeight final : public Weight {
public:
    ConstantScoreRewritingWeight(const Query &parent_query, std::shared_ptr<const MultiTermQuery> query,
                                 ScoreMode score_mode, float score)
            : parent_query_(&parent_query), query_(std::move(query)), score_mode_(score_mode), score_(score) {}

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override {
        auto scorer = const_cast<ConstantScoreRewritingWeight *>(this)->scorer(context, searcher);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        auto description = parent_query_->to_string("");
        const std::string text = description.has_value() ? description.value() : std::string("ConstantScore()");
        if (scorer.value() == nullptr) {
            return Explanation::no_match(std::format("no matching docs in {}, doc={}", text, doc), {});
        }
        if (auto advanced = scorer.value()->advance(doc); !advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (scorer.value()->doc_id() != doc) {
            return Explanation::no_match(std::format("no matching docs in {}, doc={}", text, doc), {});
        }
        auto score = scorer.value()->score();
        if (!score.has_value()) {
            return std::unexpected(std::move(score.error()));
        }
        return Explanation::match_(score.value(), text, {});
    }

    [[nodiscard]] const Query &get_query() const override { return *parent_query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher &searcher) override {
        if (context.reader() == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        auto field_terms = context.reader()->terms(query_->field());
        if (!field_terms.has_value()) {
            return std::unexpected(std::move(field_terms.error()));
        }
        Terms *terms = field_terms.value().get();
        if (terms == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        auto field_doc_count = terms->doc_count();
        if (!field_doc_count.has_value()) {
            return std::unexpected(std::move(field_doc_count.error()));
        }

        auto iterator = query_->terms_enum_for(*terms);
        if (!iterator.has_value()) {
            return std::unexpected(std::move(iterator.error()));
        }
        TermsEnum &terms_enum = *iterator.value();

        const size_t threshold = std::min(kBooleanRewriteTermCountThreshold, BooleanQuery::max_clause_count());
        std::vector<TermAndState> collected;
        bool enumeration_finished = false;
        while (collected.size() < threshold) {
            auto next = terms_enum.next();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            if (next.value() == nullptr) {
                enumeration_finished = true;
                break;
            }
            const BytesRef term = *next.value();
            auto state = terms_enum.term_state();
            if (!state.has_value()) {
                return std::unexpected(std::move(state.error()));
            }
            auto doc_freq = terms_enum.doc_freq();
            if (!doc_freq.has_value()) {
                return std::unexpected(std::move(doc_freq.error()));
            }
            auto total_term_freq = terms_enum.total_term_freq();
            if (!total_term_freq.has_value()) {
                return std::unexpected(std::move(total_term_freq.error()));
            }
            TermAndState entry(term, std::move(state.value()), doc_freq.value(), total_term_freq.value());
            if (entry.doc_freq == field_doc_count.value()) {
                // A term that contains every document with a value for the field
                // makes the other terms redundant.
                collected.clear();
                collected.push_back(std::move(entry));
                enumeration_finished = true;
                break;
            }
            collected.push_back(std::move(entry));
        }

        if (enumeration_finished) {
            if (collected.empty()) {
                return std::unique_ptr<ScorerSupplier>{};
            }
            return boolean_supplier(context, searcher, collected);
        }
        return union_supplier(context, searcher, *terms);
    }

private:
    /// Rewrites the leaf's matches into a constant score boolean query.
    ///
    /// Port of `rewrite_as_boolean_query`
    /// (`core/search/abstract_multi_term_query_constant_score_wrapper.rs`).
    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>>
    boolean_supplier(const LeafReaderContext &context, const IndexSearcher &searcher,
                     const std::vector<TermAndState> &collected) {
        BooleanQuery::Builder builder;
        for (const TermAndState &entry : collected) {
            auto states = TermStates::with_state_and_stats(searcher.reader(), entry.state, context.ord(), entry.doc_freq,
                                                           entry.total_term_freq);
            if (!states.has_value()) {
                return std::unexpected(std::move(states.error()));
            }
            auto term_query = std::make_shared<TermQuery>(Term(query_->field(), entry.term),
                                                          std::make_shared<TermStates>(std::move(states.value())));
            if (auto added = builder.add(std::move(term_query), Occur::Should); !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
        }
        auto built = builder.build();
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        auto constant_score =
                std::make_shared<ConstantScoreQuery>(std::make_shared<BooleanQuery>(std::move(built.value())));
        auto rewritten = searcher.rewrite(*constant_score);
        if (!rewritten.has_value()) {
            return std::unexpected(std::move(rewritten.error()));
        }
        // A null rewrite means the query is already in its final form.
        std::shared_ptr<Query> owned = rewritten.value();
        const Query *final_query = owned != nullptr ? owned.get() : constant_score.get();
        auto weight = final_query->create_weight(searcher, score_mode_, score_);
        if (!weight.has_value()) {
            return std::unexpected(std::move(weight.error()));
        }
        auto supplier = weight.value()->scorer_supplier(context, searcher);
        if (!supplier.has_value()) {
            return std::unexpected(std::move(supplier.error()));
        }
        return std::unique_ptr<ScorerSupplier>(
                new DelegatingScorerSupplier(std::move(weight.value()), std::move(supplier.value())));
    }

    /// Collects the documents of every matching term into one set of ids, so the
    /// clause limit cannot be hit.
    ///
    /// **Deviation:** the reference implementation keeps the enumeration it
    /// already positioned and seeks the collected terms by their states; the
    /// port restarts the enumeration instead, because some of its enums
    /// (`PredicateTermsEnum`) are forward only.
    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> union_supplier(const LeafReaderContext &context,
                                                                        const IndexSearcher &searcher, Terms &terms) {
        (void)context;
        auto iterator = query_->terms_enum_for(terms);
        if (!iterator.has_value()) {
            return std::unexpected(std::move(iterator.error()));
        }
        TermsEnum &terms_enum = *iterator.value();

        std::vector<int32_t> docs;
        while (true) {
            auto next = terms_enum.next();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            if (next.value() == nullptr) {
                break;
            }
            auto postings = terms_enum.postings_with_flags(nullptr, NONE);
            if (!postings.has_value()) {
                return std::unexpected(std::move(postings.error()));
            }
            if (postings.value() == nullptr) {
                continue;
            }
            while (true) {
                auto doc = postings.value()->next_doc();
                if (!doc.has_value()) {
                    return std::unexpected(std::move(doc.error()));
                }
                if (doc.value() == NO_MORE_DOCS) {
                    break;
                }
                docs.push_back(doc.value());
            }
        }
        std::ranges::sort(docs);
        docs.erase(std::unique(docs.begin(), docs.end()), docs.end());

        auto cost = estimate_cost(terms);
        if (!cost.has_value()) {
            return std::unexpected(std::move(cost.error()));
        }
        auto scorer = ConstantScoreScorer::from_disi(score_, score_mode_,
                                                     std::make_unique<SortedDocIdsIterator>(std::move(docs)));
        return std::unique_ptr<ScorerSupplier>(
                new ImmediateScorerSupplier(std::move(scorer), std::max<int64_t>(1, cost.value())));
    }

    /// Port of `estimate_cost`
    /// (`core/search/abstract_multi_term_query_constant_score_wrapper.rs`).
    [[nodiscard]] Result<int64_t> estimate_cost(Terms &terms) const {
        auto sum_doc_freq = terms.sum_doc_freq();
        if (!sum_doc_freq.has_value()) {
            return std::unexpected(std::move(sum_doc_freq.error()));
        }
        const int64_t query_terms_count = query_->terms_count();
        if (query_terms_count == -1) {
            return sum_doc_freq.value();
        }
        int64_t potential_extra_cost = sum_doc_freq.value();
        auto indexed_term_count = terms.size();
        if (!indexed_term_count.has_value()) {
            return std::unexpected(std::move(indexed_term_count.error()));
        }
        if (indexed_term_count.value() != -1) {
            potential_extra_cost -= indexed_term_count.value();
        }
        return query_terms_count + potential_extra_cost;
    }

    const Query *parent_query_;
    std::shared_ptr<const MultiTermQuery> query_;
    ScoreMode score_mode_;
    float score_;
};

} // namespace

const MultiTermRewriteMethod MultiTermRewriteMethod::kConstantScore{MultiTermRewriteMethod::Kind::kConstantScore};
const MultiTermRewriteMethod MultiTermRewriteMethod::kConstantScoreBlended{
        MultiTermRewriteMethod::Kind::kConstantScoreBlended};
const MultiTermRewriteMethod MultiTermRewriteMethod::kConstantScoreBoolean{
        MultiTermRewriteMethod::Kind::kConstantScoreBoolean};
const MultiTermRewriteMethod MultiTermRewriteMethod::kScoringBoolean{MultiTermRewriteMethod::Kind::kScoringBoolean};
const MultiTermRewriteMethod MultiTermRewriteMethod::kDocValues{MultiTermRewriteMethod::Kind::kDocValues};

MultiTermQuery::MultiTermQuery(std::string field, std::string pattern, MultiTermRewriteMethod rewrite_method)
        : field_(std::move(field)), pattern_(std::move(pattern)), rewrite_method_(rewrite_method) {}

bool MultiTermQuery::peers(const MultiTermQuery &other) const {
    return field_ == other.field_ && pattern_ == other.pattern_ && rewrite_method_ == other.rewrite_method_;
}

Result<std::unique_ptr<Weight>> MultiTermQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                              float boost) const {
    auto expanded = rewrite(searcher);
    if (!expanded.has_value()) {
        return std::unexpected(std::move(expanded.error()));
    }
    if (expanded.value() == nullptr) {
        return err(LuceneError::illegal_state("a multi term query has to rewrite into a concrete query"));
    }
    // The rewritten query may itself need rewriting (the top-terms rewrites
    // produce a blended term query, for instance); a null rewrite means it is
    // already in its final form.
    auto rewritten = searcher.rewrite(*expanded.value());
    if (!rewritten.has_value()) {
        return std::unexpected(std::move(rewritten.error()));
    }
    std::shared_ptr<Query> owned = rewritten.value();
    const Query *final_query = owned != nullptr ? owned.get() : expanded.value().get();
    return final_query->create_weight(searcher, score_mode, boost);
}

Result<std::vector<Term>> MultiTermQuery::expanded_terms(const IndexSearcher &searcher) const {
    return expand_terms(searcher, *this);
}

Result<std::shared_ptr<Query>> MultiTermQuery::rewrite(const IndexSearcher &searcher) const {
    switch (rewrite_method_.kind()) {
        case MultiTermRewriteMethod::Kind::kConstantScore:
        case MultiTermRewriteMethod::Kind::kConstantScoreBlended:
        case MultiTermRewriteMethod::Kind::kConstantScoreBoolean: {
            // The wrapper defers the expansion to the leaf, so a query with many
            // terms cannot overflow the clause limit.
            if (auto self = std::dynamic_pointer_cast<const MultiTermQuery>(weak_from_this().lock()); self != nullptr) {
                return std::shared_ptr<Query>(new MultiTermQueryConstantScoreWrapper(std::move(self)));
            }
            // A query that is not owned by a shared pointer cannot be wrapped.
            auto terms = expand_terms(searcher, *this);
            if (!terms.has_value()) {
                return std::unexpected(std::move(terms.error()));
            }
            return boolean_rewrite(terms.value(), true);
        }
        case MultiTermRewriteMethod::Kind::kScoringBoolean: {
            auto terms = expand_terms(searcher, *this);
            if (!terms.has_value()) {
                return std::unexpected(std::move(terms.error()));
            }
            return boolean_rewrite(terms.value(), false);
        }
        case MultiTermRewriteMethod::Kind::kDocValues: {
            // The wrapper reads the terms from the doc values of every leaf, so it
            // needs to keep the query alive.
            auto self = std::dynamic_pointer_cast<const MultiTermQuery>(weak_from_this().lock());
            if (self == nullptr) {
                return err(LuceneError::illegal_state(
                        "the doc values rewrite needs a query owned by a shared pointer"));
            }
            auto wrapper = MultiTermQueryDocValuesWrapper::create(std::move(self));
            if (!wrapper.has_value()) {
                return std::unexpected(std::move(wrapper.error()));
            }
            return std::shared_ptr<Query>(
                    new ConstantScoreQuery(std::static_pointer_cast<Query>(wrapper.value())));
        }
        case MultiTermRewriteMethod::Kind::kTopTermsBlendedFreqScoring: {
            auto rewritten = rewrite_top_terms(searcher, *this, rewrite_method_.size(),
                                               TopTermsBuildMode::kBlendedFreq);
            if (!rewritten.has_value()) {
                return std::unexpected(std::move(rewritten.error()));
            }
            return rewritten;
        }
        case MultiTermRewriteMethod::Kind::kTopTermsScoringBoolean: {
            auto rewritten = rewrite_top_terms(searcher, *this, rewrite_method_.size(),
                                               TopTermsBuildMode::kScoringBoolean);
            if (!rewritten.has_value()) {
                return std::unexpected(std::move(rewritten.error()));
            }
            return rewritten;
        }
        case MultiTermRewriteMethod::Kind::kTopTermsBoostOnlyBoolean: {
            auto rewritten = rewrite_top_terms(searcher, *this, rewrite_method_.size(),
                                               TopTermsBuildMode::kBoostOnlyBoolean);
            if (!rewritten.has_value()) {
                return std::unexpected(std::move(rewritten.error()));
            }
            return rewritten;
        }
    }
    return err(LuceneError::illegal_state("unknown multi term rewrite method"));
}

VoidResult MultiTermQuery::visit(QueryVisitor &visitor) const {
    if (!visitor.accept_field(field_)) {
        return {};
    }
    return visitor.visit_leaf(*this);
}

Result<std::string> MultiTermQuery::to_string(std::string_view field) const {
    const std::string body = std::format("{}:{}({})", query_name(), pattern_.empty() ? field_ : field_, pattern_);
    if (!field.empty() && field == field_) {
        return std::format("{}({})", query_name(), pattern_);
    }
    return body;
}

bool MultiTermQuery::equals(const Query &other) const {
    const auto *multi_term = dynamic_cast<const MultiTermQuery *>(&other);
    if (multi_term == nullptr) {
        return false;
    }
    if (typeid(*this) != typeid(other)) {
        return false;
    }
    return peers(*multi_term);
}

size_t MultiTermQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    result = result * 31 + std::hash<std::string>{}(pattern_);
    result = result * 31 + rewrite_method_.hash();
    return result;
}

// ------------------------------------- MultiTermQueryConstantScoreWrapper

MultiTermQueryConstantScoreWrapper::MultiTermQueryConstantScoreWrapper(std::shared_ptr<const MultiTermQuery> query)
        : query_(std::move(query)) {
    debug_check(query_ != nullptr, "a constant score wrapper needs a multi term query");
}

Result<std::unique_ptr<Weight>> MultiTermQueryConstantScoreWrapper::create_weight(const IndexSearcher &searcher,
                                                                                 ScoreMode score_mode,
                                                                                 float boost) const {
    (void)searcher;
    if (query_ == nullptr) {
        return err(LuceneError::illegal_state("the constant score wrapper has no query"));
    }
    std::unique_ptr<Weight> weight(new ConstantScoreRewritingWeight(*this, query_, score_mode, boost));
    return weight;
}

VoidResult MultiTermQueryConstantScoreWrapper::visit(QueryVisitor &visitor) const {
    if (query_ == nullptr) {
        return {};
    }
    // **Deviation:** the reference implementation visits through a sub visitor for
    // the `FILTER` occurrence; the port has no `get_sub_visitor` yet.
    return query_->visit(visitor);
}

Result<std::string> MultiTermQueryConstantScoreWrapper::to_string(std::string_view field) const {
    if (query_ == nullptr) {
        return std::string("ConstantScore()");
    }
    return query_->to_string(field);
}

bool MultiTermQueryConstantScoreWrapper::equals(const Query &other) const {
    const auto *wrapper = dynamic_cast<const MultiTermQueryConstantScoreWrapper *>(&other);
    if (wrapper == nullptr || query_ == nullptr || wrapper->query_ == nullptr) {
        return false;
    }
    return query_->equals(*wrapper->query_);
}

size_t MultiTermQueryConstantScoreWrapper::hash() const {
    if (query_ == nullptr) {
        return 0;
    }
    return query_->hash() * 31 + 0x5BF03635;
}

// ------------------------------------------------------- PredicateTermsEnum

PredicateTermsEnum::PredicateTermsEnum(std::unique_ptr<TermsEnum> inner, Judge accept, Judge stop)
        : inner_(std::move(inner)), accept_(std::move(accept)), stop_(std::move(stop)) {}

Result<const BytesRef *> PredicateTermsEnum::next() {
    if (ended_) {
        return static_cast<const BytesRef *>(nullptr);
    }
    while (true) {
        auto next_term = inner_->next();
        if (!next_term.has_value()) {
            return std::unexpected(std::move(next_term.error()));
        }
        if (next_term.value() == nullptr) {
            ended_ = true;
            return static_cast<const BytesRef *>(nullptr);
        }
        const BytesRef &candidate = *next_term.value();
        if (accept_(candidate)) {
            return &candidate;
        }
        if (stop_(candidate)) {
            // No later term can match, because terms are sorted.
            ended_ = true;
            return static_cast<const BytesRef *>(nullptr);
        }
    }
}

Result<const BytesRef *> PredicateTermsEnum::term() const { return inner_->term(); }

Result<int64_t> PredicateTermsEnum::ord() const { return inner_->ord(); }

Result<int32_t> PredicateTermsEnum::doc_freq() { return inner_->doc_freq(); }

Result<int64_t> PredicateTermsEnum::total_term_freq() { return inner_->total_term_freq(); }

Result<std::unique_ptr<PostingsEnum>> PredicateTermsEnum::postings_with_flags(std::unique_ptr<PostingsEnum> reuse,
                                                                             int32_t flags) {
    return inner_->postings_with_flags(std::move(reuse), flags);
}

Result<std::unique_ptr<ImpactsEnum>> PredicateTermsEnum::impacts(int32_t flags) { return inner_->impacts(flags); }

Result<AttributeSource *> PredicateTermsEnum::attributes() { return inner_->attributes(); }

Result<AttributeSource *> PredicateTermsEnum::attributes_mut() { return inner_->attributes_mut(); }

Result<bool> PredicateTermsEnum::seek_exact(std::span<const uint8_t> /*target*/) {
    return err(LuceneError::unsupported_operation("PredicateTermsEnum is forward only"));
}

Result<bool> PredicateTermsEnum::prepare_seek_exact(std::span<const uint8_t> /*target*/) {
    return err(LuceneError::unsupported_operation("PredicateTermsEnum is forward only"));
}

Result<bool> PredicateTermsEnum::get_prepare_seek_exact_status(std::span<const uint8_t> /*target*/) {
    return err(LuceneError::unsupported_operation("PredicateTermsEnum is forward only"));
}

Result<SeekStatus> PredicateTermsEnum::seek_ceil(std::span<const uint8_t> /*target*/) {
    return err(LuceneError::unsupported_operation("PredicateTermsEnum is forward only"));
}

VoidResult PredicateTermsEnum::seek_exact_with_ord(int64_t /*ord*/) {
    return err(LuceneError::unsupported_operation("PredicateTermsEnum is forward only"));
}

VoidResult PredicateTermsEnum::seek_exact_with_state(std::span<const uint8_t> /*term*/, const TermStateEnum & /*state*/) {
    return err(LuceneError::unsupported_operation("PredicateTermsEnum is forward only"));
}

Result<TermStateEnum> PredicateTermsEnum::term_state() { return inner_->term_state(); }

} // namespace lucene
