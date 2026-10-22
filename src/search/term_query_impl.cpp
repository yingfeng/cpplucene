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

module lucene_core:term_query.impl;

import :term_query;

import :collection_statistics;
import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :number;
import :numeric_doc_values;
import :postings_enum;
import :query;
import :query_visitor;
import :reader_util;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :similarity;
import :term;
import :term_scorer;
import :term_states;
import :term_statistics;
import :terms;
import :terms_enum;
import :types;
import :weight;

import std;

namespace lucene {

namespace {

/// Scores nothing; used when a query's scores are not needed, so that no
/// collector-level scorer (and for BM25 no 256-entry table) is allocated.
/// rlucene's `SimScorerImpl` in `term_query.rs`.
class DummySimScorer final : public SimScorer {
public:
    [[nodiscard]] float score(float /*freq*/, int64_t /*norm*/) const override { return 0.0F; }
};

/// The score-less similarity scorer of rlucene's `create_weight` fast path.
std::shared_ptr<SimScorer> dummy_sim_scorer() {
    static const auto instance = std::make_shared<DummySimScorer>();
    return instance;
}

} // namespace

// ---------------------------------------------------------------- TermQuery

TermQuery::TermQuery(Term term) : term_(std::move(term)) {}

TermQuery::TermQuery(Term term, std::shared_ptr<TermStates> per_reader_term_state)
        : term_(std::move(term)), per_reader_term_state_(std::move(per_reader_term_state)) {}

Result<std::unique_ptr<Weight>> TermQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                         float boost) const {
    std::shared_ptr<TermStates> states;
    if (per_reader_term_state_ != nullptr && per_reader_term_state_->was_built_for_id(searcher.top_reader_identity())) {
        states = per_reader_term_state_;
    } else {
        auto built = build_term_states(searcher, term_, score_mode_needs_scores(score_mode));
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        states = std::make_shared<TermStates>(std::move(built.value()));
    }
    auto weight = TermWeight::create(*this, term_, searcher, score_mode, boost, std::move(states));
    if (!weight.has_value()) {
        return std::unexpected(std::move(weight.error()));
    }
    std::unique_ptr<Weight> result = std::move(weight.value());
    return result;
}

VoidResult TermQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(term_.field())) {
        const std::array<Term, 1> terms{term_};
        return visitor.consume_terms(*this, terms);
    }
    return {};
}

Result<std::string> TermQuery::to_string(std::string_view field) const {
    if (term_.field() == field) {
        return term_.text();
    }
    return term_.field() + ":" + term_.text();
}

bool TermQuery::equals(const Query &other) const {
    const auto *term_query = dynamic_cast<const TermQuery *>(&other);
    if (term_query == nullptr) {
        return false;
    }
    return term_ == term_query->term_;
}

size_t TermQuery::hash() const {
    size_t result = std::hash<std::string>{}(term_.field());
    for (const auto byte : term_.bytes().as_slice()) {
        result = result * 31 + byte;
    }
    return result;
}

// --------------------------------------------------------------- TermWeight

TermWeight::TermWeight(const Query &parent_query, Term term, std::shared_ptr<Similarity> similarity,
                       std::shared_ptr<SimScorer> sim_scorer, std::shared_ptr<TermStates> term_states,
                       ScoreMode score_mode)
        : parent_query_(&parent_query),
          term_(std::move(term)),
          similarity_(std::move(similarity)),
          sim_scorer_(std::move(sim_scorer)),
          term_states_(std::move(term_states)),
          score_mode_(score_mode) {}

Result<std::unique_ptr<TermWeight>> TermWeight::create(const Query &parent_query, const Term &term,
                                                       const IndexSearcher &searcher, ScoreMode score_mode, float boost,
                                                       std::shared_ptr<TermStates> term_states) {
    const bool needs_scores = score_mode_needs_scores(score_mode);

    // Without scores the statistics are irrelevant: fake them, so that a
    // similarity is never asked for a field it cannot see.
    std::optional<CollectionStatistics> collection_stats;
    std::optional<TermStatistics> term_stats;
    if (needs_scores) {
        auto stats = searcher.collection_statistics(term.field());
        if (!stats.has_value()) {
            return std::unexpected(std::move(stats.error()));
        }
        collection_stats = std::move(stats.value());
        auto doc_freq = term_states->doc_freq();
        if (!doc_freq.has_value()) {
            return std::unexpected(std::move(doc_freq.error()));
        }
        if (doc_freq.value() > 0) {
            auto total_term_freq = term_states->total_term_freq();
            if (!total_term_freq.has_value()) {
                return std::unexpected(std::move(total_term_freq.error()));
            }
            auto created = searcher.term_statistics(term, doc_freq.value(), total_term_freq.value());
            if (!created.has_value()) {
                return std::unexpected(std::move(created.error()));
            }
            term_stats = std::move(created.value());
        }
    } else {
        auto created = CollectionStatistics::create(term.field(), 1, 1, 1, 1);
        if (!created.has_value()) {
            return std::unexpected(std::move(created.error()));
        }
        collection_stats = std::move(created.value());
        auto created_term = TermStatistics::create(term, 1, 1);
        if (!created_term.has_value()) {
            return std::unexpected(std::move(created_term.error()));
        }
        term_stats = std::move(created_term.value());
    }

    std::shared_ptr<SimScorer> sim_scorer;
    if (term_stats.has_value()) {
        if (needs_scores) {
            if (!collection_stats.has_value()) {
                return err(LuceneError::illegal_state("term query collection statistics are missing"));
            }
            const std::array<TermStatistics, 1> stats{*term_stats};
            auto scorer = searcher.similarity().scorer(boost, *collection_stats, stats);
            if (!scorer.has_value()) {
                return std::unexpected(std::move(scorer.error()));
            }
            sim_scorer = std::move(scorer.value());
        } else {
            sim_scorer = dummy_sim_scorer();
        }
    }

    return std::unique_ptr<TermWeight>(new TermWeight(parent_query, term, searcher.similarity_ptr(), sim_scorer,
                                                     std::move(term_states), score_mode));
}

Result<std::unique_ptr<TermsEnum>> TermWeight::get_terms_enum(const LeafReaderContext &context) const {
    // rlucene asserts that the states were built for this reader tree.
    if (!term_states_->was_built_for(ReaderUtil::get_top_level_context(context))) {
        return err(LuceneError::illegal_state(
                "The top-reader used to create Weight is not the same as the current reader's top-reader"));
    }

    auto prepared = term_states_->get(context);
    if (!prepared.has_value()) {
        return std::unexpected(std::move(prepared.error()));
    }
    std::optional<const TermStateEnum *> state;
    if (prepared.value().has_value()) {
        auto resolved = term_states_->resolve(*prepared.value());
        if (!resolved.has_value()) {
            return std::unexpected(std::move(resolved.error()));
        }
        state = resolved.value();
    }
    if (!state.has_value()) {
        // The term is not in this leaf; the invariant is checked so that a
        // missing term state never silently turns into "no matches".
        auto absent = term_not_in_reader(context);
        if (!absent.has_value()) {
            return std::unexpected(std::move(absent.error()));
        }
        if (!absent.value()) {
            return err(LuceneError::illegal_state("no termstate found but term exists in reader"));
        }
        return std::unique_ptr<TermsEnum>{};
    }

    auto terms = context.reader()->terms(term_.field());
    if (!terms.has_value()) {
        return std::unexpected(std::move(terms.error()));
    }
    if (terms.value() == nullptr) {
        return err(LuceneError::illegal_state("term state exists but field terms are missing"));
    }
    auto terms_enum = terms.value()->iterator();
    if (!terms_enum.has_value()) {
        return std::unexpected(std::move(terms_enum.error()));
    }
    auto sought = terms_enum.value()->seek_exact_with_state(term_.bytes().as_slice(), *state.value());
    if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
    }
    return terms_enum;
}

Result<bool> TermWeight::term_not_in_reader(const LeafReaderContext &context) const {
    auto doc_freq = context.reader()->doc_freq(term_);
    if (!doc_freq.has_value()) {
        return std::unexpected(std::move(doc_freq.error()));
    }
    return doc_freq.value() == 0;
}

Result<Explanation> TermWeight::explain(const LeafReaderContext &context, int32_t doc,
                                        const IndexSearcher &searcher) const {
    (void)searcher;
    auto terms_enum = get_terms_enum(context);
    if (!terms_enum.has_value()) {
        return std::unexpected(std::move(terms_enum.error()));
    }
    if (terms_enum.value() == nullptr) {
        return Explanation::no_match_no_details("no matching term");
    }

    auto postings = terms_enum.value()->postings_with_flags(nullptr, FREQS);
    if (!postings.has_value()) {
        return std::unexpected(std::move(postings.error()));
    }
    auto advanced = postings.value()->advance(doc);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    if (advanced.value() != doc) {
        return Explanation::no_match_no_details("no matching term");
    }

    auto freq = postings.value()->freq();
    if (!freq.has_value()) {
        return std::unexpected(std::move(freq.error()));
    }

    int64_t norm = 1;
    auto norms = context.reader()->get_norm_values(term_.field());
    if (!norms.has_value()) {
        return std::unexpected(std::move(norms.error()));
    }
    if (norms.value() != nullptr) {
        auto exact = norms.value()->advance_exact(doc);
        if (!exact.has_value()) {
            return std::unexpected(std::move(exact.error()));
        }
        if (exact.value()) {
            auto value = norms.value()->long_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            norm = value.value();
        }
    }

    if (sim_scorer_ == nullptr) {
        return err(LuceneError::illegal_state("term scorer is missing"));
    }
    const Explanation freq_explanation =
            Explanation::match_no_details(freq.value(), "freq, occurrences of term within document");
    auto score_explanation = sim_scorer_->explain(freq_explanation, norm);
    if (!score_explanation.has_value()) {
        return std::unexpected(std::move(score_explanation.error()));
    }

    auto query_text = parent_query_->to_string("");
    const std::string description = "weight(" + (query_text.has_value() ? query_text.value() : std::string("?")) +
                                    " in " + std::to_string(doc) + ") [" + similarity_->name() + "], result of:";
    return Explanation::match_(score_explanation.value().value(), description,
                               std::vector<Explanation>{std::move(score_explanation.value())});
}

Result<std::unique_ptr<ScorerSupplier>> TermWeight::scorer_supplier(const LeafReaderContext &context,
                                                                   const IndexSearcher &searcher) {
    (void)searcher;
    if (!term_states_->was_built_for(ReaderUtil::get_top_level_context(context))) {
        return err(LuceneError::illegal_state(
                "The top-reader used to create Weight is not the same as the current reader's top-reader"));
    }
    auto prepared = term_states_->get(context);
    if (!prepared.has_value()) {
        return std::unexpected(std::move(prepared.error()));
    }
    if (!prepared.value().has_value()) {
        return std::unique_ptr<ScorerSupplier>{};
    }
    if (sim_scorer_ == nullptr) {
        return err(LuceneError::illegal_state("term scorer is missing"));
    }
    std::unique_ptr<ScorerSupplier> supplier = std::make_unique<TermScorerSupplier>(
            /*top_level_scoring_clause=*/false, term_states_, std::move(prepared.value().value()), term_, sim_scorer_,
            score_mode_);
    return supplier;
}

Result<int32_t> TermWeight::count(const LeafReaderContext &context, const IndexSearcher &searcher) {
    (void)searcher;
    auto deletions = context.reader()->has_deletions();
    if (!deletions.has_value()) {
        return std::unexpected(std::move(deletions.error()));
    }
    if (deletions.value()) {
        // Deleted documents are included in the term statistics, so they have to
        // be visited.
        return default_count(context);
    }
    auto terms_enum = get_terms_enum(context);
    if (!terms_enum.has_value()) {
        return std::unexpected(std::move(terms_enum.error()));
    }
    if (terms_enum.value() == nullptr) {
        return 0;
    }
    return terms_enum.value()->doc_freq();
}

// ------------------------------------------------------- TermScorerSupplier

TermScorerSupplier::TermScorerSupplier(bool top_level_scoring_clause, std::shared_ptr<TermStates> term_states,
                                       PrepareState prepare_state, Term term, std::shared_ptr<SimScorer> sim_scorer,
                                       ScoreMode score_mode)
        : top_level_scoring_clause_(top_level_scoring_clause),
          term_states_(std::move(term_states)),
          prepare_state_(std::move(prepare_state)),
          term_(std::move(term)),
          sim_scorer_(std::move(sim_scorer)),
          score_mode_(score_mode) {}

Result<bool> TermScorerSupplier::prepare(const LeafReaderContext &context) {
    if (prepared_) {
        return present_;
    }
    prepared_ = true;
    present_ = false;

    auto state = term_states_->resolve(prepare_state_);
    if (!state.has_value()) {
        return std::unexpected(std::move(state.error()));
    }
    if (!state.value().has_value()) {
        return false;
    }
    const TermStateEnum *term_state = state.value().value();
    auto terms = context.reader()->terms(term_.field());
    if (!terms.has_value()) {
        return std::unexpected(std::move(terms.error()));
    }
    if (terms.value() == nullptr) {
        return err(LuceneError::illegal_argument("term should exist here " + term_.to_string()));
    }
    auto terms_enum = terms.value()->iterator();
    if (!terms_enum.has_value()) {
        return std::unexpected(std::move(terms_enum.error()));
    }
    auto sought = terms_enum.value()->seek_exact_with_state(term_.bytes().as_slice(), *term_state);
    if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
    }
    terms_enum_ = std::move(terms_enum.value());
    present_ = true;
    return true;
}

Result<std::unique_ptr<Scorer>> TermScorerSupplier::get(int64_t lead_cost, const LeafReaderContext &context,
                                                        const IndexSearcher &searcher) {
    (void)lead_cost;
    (void)searcher;
    auto present = prepare(context);
    if (!present.has_value()) {
        return std::unexpected(std::move(present.error()));
    }
    return build_term_scorer(context, present.value() ? terms_enum_.get() : nullptr, term_, sim_scorer_, score_mode_);
}

Result<int64_t> TermScorerSupplier::cost(const LeafReaderContext &context, const IndexSearcher &searcher) {
    (void)searcher;
    auto present = prepare(context);
    if (!present.has_value()) {
        return std::unexpected(std::move(present.error()));
    }
    if (!present.value()) {
        return int64_t{0};
    }
    auto doc_freq = terms_enum_->doc_freq();
    if (!doc_freq.has_value()) {
        return std::unexpected(std::move(doc_freq.error()));
    }
    return static_cast<int64_t>(doc_freq.value());
}

VoidResult TermScorerSupplier::set_top_level_scoring_clause() {
    top_level_scoring_clause_ = true;
    return {};
}

// ------------------------------------------------------------ shared helper

Result<std::unique_ptr<Scorer>> build_term_scorer(const LeafReaderContext &context, TermsEnum *terms_enum,
                                                  const Term &term, std::shared_ptr<SimScorer> sim_scorer,
                                                  ScoreMode score_mode) {
    if (terms_enum == nullptr) {
        // No postings for this leaf: an empty scorer with score 0 keeps the
        // "term exists elsewhere" case uniform.
        std::unique_ptr<DocIdSetIterator> empty = std::make_unique<EmptyDISI>();
        std::unique_ptr<Scorer> scorer = ConstantScoreScorer::from_disi(0.0F, score_mode, std::move(empty));
        return scorer;
    }
    if (sim_scorer == nullptr) {
        return err(LuceneError::illegal_state("TermQuery similarity scorer is missing"));
    }

    std::unique_ptr<NumericDocValues> norms;
    if (score_mode_needs_scores(score_mode)) {
        auto values = context.reader()->get_norm_values(term.field());
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        norms = std::move(values.value());
    }

    const int32_t flags = score_mode_needs_scores(score_mode) ? FREQS : NONE;
    auto postings = terms_enum->postings_with_flags(nullptr, flags);
    if (!postings.has_value()) {
        return std::unexpected(std::move(postings.error()));
    }
    std::unique_ptr<Scorer> scorer = std::make_unique<TermScorer>(std::move(postings.value()), std::move(sim_scorer),
                                                                 std::move(norms));
    return scorer;
}

} // namespace lucene
