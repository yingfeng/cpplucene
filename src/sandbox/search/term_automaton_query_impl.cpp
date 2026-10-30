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

module lucene_core:term_automaton_query.impl;

import :term_automaton_query;

import std;

import :automaton;
import :block_term_state;
import :bytes_ref;
import :collection_statistics;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :index_searcher;
import :ints_ref;
import :leaf_reader;
import :leaf_reader_context;
import :match_no_docs_query;
import :multi_phrase_query;
import :numeric_doc_values;
import :occur;
import :operations;
import :phrase_query;
import :postings_enum;
import :query;
import :query_visitor;
import :run_automaton;
import :score_mode;
import :scorable;
import :scorer;
import :scorer_supplier;
import :similarity;
import :term;
import :term_automaton_scorer;
import :term_query;
import :term_states;
import :term_statistics;
import :terms;
import :terms_enum;
import :transition;
import :types;
import :weight;

namespace lucene {

namespace {

/// Scores nothing, for the case where the caller asked for no scores.
class TermAutomatonDummySimScorer final : public SimScorer {
public:
    [[nodiscard]] float score(float /*freq*/, int64_t /*norm*/) const override { return 0.0F; }
};

std::shared_ptr<const SimScorer> dummy_sim_scorer() {
    static const auto instance = std::make_shared<const TermAutomatonDummySimScorer>();
    return instance;
}

/// Supplies the (single) scorer of one leaf; the scorer is built on demand so
/// that a leaf where none of the terms occurs costs nothing.
class TermAutomatonScorerSupplier final : public ScorerSupplier {
public:
    explicit TermAutomatonScorerSupplier(const TermAutomatonWeight &weight) : weight_(weight) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext &context,
                                                      const IndexSearcher & /*searcher*/) override {
        auto scorer = weight_.build_scorer(context);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        if (scorer.value() == nullptr) {
            return std::unique_ptr<Scorer>{};
        }
        return std::unique_ptr<Scorer>(std::move(scorer.value()));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        // Building the scorer is the only way to know its cost, which defeats
        // the purpose; report "unknown" (the largest possible value).
        return std::numeric_limits<int64_t>::max();
    }

private:
    const TermAutomatonWeight &weight_;
};

} // namespace

// ------------------------------------------------------------ TermAutomatonQuery

TermAutomatonQuery::TermAutomatonQuery(std::string field) : field_(std::move(field)) {}

int32_t TermAutomatonQuery::create_state() { return builder_.create_state(); }

void TermAutomatonQuery::set_accept(int32_t state, bool accept) { builder_.set_accept(state, accept); }

VoidResult TermAutomatonQuery::add_transition(int32_t source, int32_t dest, std::string_view term) {
    return add_transition_bytes(source, dest,
                                std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(term.data()), term.size()));
}

VoidResult TermAutomatonQuery::add_transition_bytes(int32_t source, int32_t dest, std::span<const uint8_t> term) {
    auto term_id = get_term_id(term);
    if (!term_id.has_value()) {
        return std::unexpected(std::move(term_id.error()));
    }
    return builder_.add_transition_label(source, dest, term_id.value());
}

VoidResult TermAutomatonQuery::add_any_transition(int32_t source, int32_t dest) {
    auto term_id = get_term_id(std::nullopt);
    if (!term_id.has_value()) {
        return std::unexpected(std::move(term_id.error()));
    }
    return builder_.add_transition_label(source, dest, term_id.value());
}

Result<int32_t> TermAutomatonQuery::get_term_id(std::optional<std::span<const uint8_t>> term) {
    if (!term.has_value()) {
        if (any_term_id_ == -1) {
            any_term_id_ = static_cast<int32_t>(id_to_term_.size());
            id_to_term_.emplace_back(std::nullopt);
        }
        return any_term_id_;
    }
    std::string key(reinterpret_cast<const char *>(term.value().data()), term.value().size());
    if (const auto found = term_to_id_.find(key); found != term_to_id_.end()) {
        return found->second;
    }
    const auto id = static_cast<int32_t>(id_to_term_.size());
    term_to_id_.emplace(key, id);
    id_to_term_.emplace_back(std::move(key));
    return id;
}

VoidResult TermAutomatonQuery::finish() { return finish_with_work_limit(Operations::DEFAULT_DETERMINIZE_WORK_LIMIT); }

VoidResult TermAutomatonQuery::finish_with_work_limit(size_t determinize_work_limit) {
    auto built = builder_.finish();
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    Automaton automaton = std::move(built.value());
    Transition transition;

    if (any_term_id_ != -1) {
        // Make sure there are no leading or trailing ANY transitions.
        const int32_t initial_count = automaton.init_transition(0, transition);
        for (int32_t i = 0; i < initial_count; ++i) {
            if (auto next = automaton.get_next_transition(transition); !next.has_value()) {
                return next;
            }
            if (any_term_id_ >= transition.min && any_term_id_ <= transition.max) {
                return err(LuceneError::illegal_state("automaton cannot lead with an ANY transition"));
            }
        }

        const int32_t num_states = automaton.get_num_states();
        for (int32_t state = 0; state < num_states; ++state) {
            const int32_t count = automaton.init_transition(state, transition);
            for (int32_t i = 0; i < count; ++i) {
                if (auto next = automaton.get_next_transition(transition); !next.has_value()) {
                    return next;
                }
                if (automaton.is_accept(transition.dest) && any_term_id_ >= transition.min &&
                    any_term_id_ <= transition.max) {
                    return err(LuceneError::illegal_state("automaton cannot end with an ANY transition"));
                }
            }
        }

        const int32_t term_count = static_cast<int32_t>(id_to_term_.size());

        // The wildcard has to match every other term, so its transition is
        // widened to the whole alphabet.
        Automaton widened;
        for (int32_t state = 0; state < num_states; ++state) {
            (void)widened.create_state();
            widened.set_accept(state, automaton.is_accept(state));
        }
        for (int32_t state = 0; state < num_states; ++state) {
            const int32_t count = automaton.init_transition(state, transition);
            for (int32_t i = 0; i < count; ++i) {
                if (auto next = automaton.get_next_transition(transition); !next.has_value()) {
                    return next;
                }
                int32_t min = transition.min;
                int32_t max = transition.max;
                if (transition.min <= any_term_id_ && any_term_id_ <= transition.max) {
                    min = 0;
                    max = term_count - 1;
                }
                if (auto added = widened.add_transition(transition.source, transition.dest, min, max);
                    !added.has_value()) {
                    return added;
                }
            }
        }
        if (auto finished = widened.finish_state(); !finished.has_value()) {
            return finished;
        }
        automaton = std::move(widened);
    }

    auto deterministic = Operations::determinize(automaton, determinize_work_limit);
    if (!deterministic.has_value()) {
        return std::unexpected(std::move(deterministic.error()));
    }
    if (deterministic.value().has_value()) {
        automaton = std::move(deterministic.value().value());
    }

    auto pruned = Operations::remove_dead_states(automaton);
    if (!pruned.has_value()) {
        return std::unexpected(std::move(pruned.error()));
    }
    if (pruned.value().has_value()) {
        automaton = std::move(pruned.value().value());
    }

    if (automaton.is_accept(0)) {
        return err(LuceneError::illegal_state("cannot accept the empty string"));
    }
    determinized_ = std::move(automaton);
    return {};
}

const Automaton &TermAutomatonQuery::determinized() const { return require_determinized(); }

const Automaton &TermAutomatonQuery::require_determinized() const {
    if (!determinized_.has_value()) {
        throw std::logic_error("call finish first on a TermAutomatonQuery");
    }
    return determinized_.value();
}

std::optional<BytesRef> TermAutomatonQuery::term_bytes(int32_t term_id) const {
    if (term_id < 0 || static_cast<size_t>(term_id) >= id_to_term_.size()) {
        return std::nullopt;
    }
    const std::optional<std::string> &term = id_to_term_[static_cast<size_t>(term_id)];
    if (!term.has_value()) {
        return std::nullopt;
    }
    return BytesRef::from_string(term.value());
}

Result<std::string> TermAutomatonQuery::to_dot() const {
    const Automaton &det = require_determinized();
    std::string buffer = "digraph Automaton {\n  rankdir = LR\n";
    const int32_t num_states = det.get_num_states();
    if (num_states > 0) {
        buffer += "  initial [shape=plaintext,label=\"0\"]\n  initial -> 0\n";
    }

    Transition transition;
    for (int32_t state = 0; state < num_states; ++state) {
        buffer += "  " + std::to_string(state);
        buffer += det.is_accept(state) ? " [shape=doublecircle,label=\"" : " [shape=circle,label=\"";
        buffer += std::to_string(state);
        buffer += "\"]\n";
        const int32_t count = det.init_transition(state, transition);
        for (int32_t i = 0; i < count; ++i) {
            if (auto next = det.get_next_transition(transition); !next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            for (int32_t term_id = transition.min; term_id <= transition.max; ++term_id) {
                buffer += "  " + std::to_string(state) + " -> " + std::to_string(transition.dest) + " [label=\"";
                if (term_id == any_term_id_) {
                    buffer += '*';
                } else {
                    const std::optional<BytesRef> term = term_bytes(term_id);
                    if (!term.has_value()) {
                        return err(LuceneError::illegal_state("term id has no term"));
                    }
                    auto text = term.value().utf8_to_string();
                    if (!text.has_value()) {
                        return std::unexpected(std::move(text.error()));
                    }
                    buffer += text.value();
                }
                buffer += "\"]\n";
            }
        }
    }
    buffer += '}';
    return buffer;
}

Result<std::unique_ptr<Weight>> TermAutomatonQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                  float boost) const {
    auto weight = TermAutomatonWeight::create(*this, boost, score_mode, searcher);
    if (!weight.has_value()) {
        return std::unexpected(std::move(weight.error()));
    }
    return std::unique_ptr<Weight>(std::move(weight.value()));
}

Result<std::shared_ptr<Query>> TermAutomatonQuery::rewrite(const IndexSearcher &searcher) const {
    (void)searcher;
    const Automaton &det = require_determinized();

    auto empty = Operations::is_empty(det);
    if (!empty.has_value()) {
        return std::unexpected(std::move(empty.error()));
    }
    if (empty.value()) {
        return std::shared_ptr<Query>(std::make_shared<MatchNoDocsQuery>());
    }

    auto singleton = Operations::get_singleton(det);
    if (!singleton.has_value()) {
        return std::unexpected(std::move(singleton.error()));
    }
    if (singleton.value().has_value() && singleton.value().value().length() == 1) {
        const int32_t term_id = singleton.value().value().as_slice()[0];
        auto bytes = term_bytes(term_id);
        if (!bytes.has_value()) {
            return err(LuceneError::illegal_state("singleton term is ANY"));
        }
        return std::shared_ptr<Query>(std::make_shared<TermQuery>(Term(field_, std::move(bytes.value()))));
    }

    // Try for either a PhraseQuery or a MultiPhraseQuery, which only works when
    // the automaton is a "sausage": every state has transitions to one single
    // destination.
    std::optional<MultiPhraseQuery::Builder> multi_phrase{std::in_place};
    bool phrase_rejected = false;
    std::vector<Term> phrase_terms;
    std::vector<int32_t> phrase_positions;

    Transition transition;
    int32_t state = 0;
    int32_t position = 0;
    while (true) {
        const int32_t count = det.init_transition(state, transition);
        if (count == 0) {
            if (!det.is_accept(state)) {
                multi_phrase.reset();
                phrase_rejected = true;
            }
            break;
        }
        if (det.is_accept(state)) {
            multi_phrase.reset();
            phrase_rejected = true;
            break;
        }

        int32_t dest = -1;
        std::vector<std::pair<int32_t, int32_t>> ranges;
        bool matches_any = false;
        bool diverged = false;
        for (int32_t i = 0; i < count; ++i) {
            if (auto next = det.get_next_transition(transition); !next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            if (i == 0) {
                dest = transition.dest;
            } else if (dest != transition.dest) {
                diverged = true;
                break;
            }
            matches_any |= any_term_id_ >= transition.min && any_term_id_ <= transition.max;
            ranges.emplace_back(transition.min, transition.max);
        }
        if (diverged) {
            multi_phrase.reset();
            phrase_rejected = true;
            break;
        }

        if (!matches_any) {
            std::vector<Term> terms;
            for (const auto &[min, max] : ranges) {
                for (int32_t term_id = min; term_id <= max; ++term_id) {
                    auto bytes = term_bytes(term_id);
                    if (!bytes.has_value()) {
                        return err(LuceneError::illegal_state("term id has no term"));
                    }
                    terms.emplace_back(field_, std::move(bytes.value()));
                }
            }
            if (!phrase_rejected) {
                if (terms.size() == 1) {
                    phrase_terms.push_back(terms.front());
                    phrase_positions.push_back(position);
                } else {
                    phrase_rejected = true;
                }
            }
            if (multi_phrase.has_value()) {
                if (auto added = multi_phrase->add_terms_with_position(std::move(terms), position);
                    !added.has_value()) {
                    return std::unexpected(std::move(added.error()));
                }
            }
        }

        state = dest;
        ++position;
    }

    if (!phrase_rejected && !phrase_terms.empty()) {
        auto phrase = PhraseQuery::with_positions(field_, phrase_terms, phrase_positions, PhraseQuery::DEFAULT_SLOP);
        if (!phrase.has_value()) {
            return std::unexpected(std::move(phrase.error()));
        }
        return std::shared_ptr<Query>(std::make_shared<PhraseQuery>(std::move(phrase.value())));
    }
    if (multi_phrase.has_value()) {
        auto built = multi_phrase->build();
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        return std::shared_ptr<Query>(std::make_shared<MultiPhraseQuery>(std::move(built.value())));
    }
    return std::shared_ptr<Query>{};
}

VoidResult TermAutomatonQuery::visit(QueryVisitor &visitor) const {
    if (!visitor.accept_field(field_)) {
        return {};
    }
    std::vector<Term> terms;
    terms.reserve(id_to_term_.size());
    for (const std::optional<std::string> &term : id_to_term_) {
        if (!term.has_value()) {
            continue;
        }
        terms.emplace_back(field_, BytesRef::from_string(term.value()));
    }
    DefaultQueryVisitor sub = visitor.get_sub_visitor(Occur::Should, *this);
    return sub.consume_terms(*this, terms);
}

Result<std::string> TermAutomatonQuery::to_string(std::string_view field) const {
    std::string buffer = "TermAutomatonQuery(field=";
    buffer += field_;
    if (determinized_.has_value()) {
        buffer += " numStates=";
        buffer += std::to_string(determinized_.value().get_num_states());
    }
    buffer += ')';
    return buffer;
}

bool TermAutomatonQuery::equals(const Query &other) const {
    return identity().is_same(other.identity());
}

size_t TermAutomatonQuery::hash() const { return identity().hash(); }

// ----------------------------------------------------------- TermAutomatonWeight

TermAutomatonWeight::TermAutomatonWeight(const TermAutomatonQuery &query, float boost, ScoreMode score_mode,
                                        std::shared_ptr<const Similarity> similarity)
        : query_(query), boost_(boost), score_mode_(score_mode), similarity_(std::move(similarity)) {}

Result<std::unique_ptr<TermAutomatonWeight>> TermAutomatonWeight::create(const TermAutomatonQuery &query, float boost,
                                                                        ScoreMode score_mode,
                                                                        const IndexSearcher &searcher) {
    if (!query.is_finished()) {
        return err(LuceneError::illegal_state("call finish first on the TermAutomatonQuery"));
    }
    std::unique_ptr<TermAutomatonWeight> weight(
            new TermAutomatonWeight(query, boost, score_mode, searcher.similarity_ptr()));

    // The alphabet of the run automaton is the set of term ids; a query without
    // any term still needs a non-empty alphabet to build the tables.
    auto run = RunAutomaton::create(Automaton(query.determinized()),
                                    std::max<size_t>(1, static_cast<size_t>(query.num_term_ids())));
    if (!run.has_value()) {
        return std::unexpected(std::move(run.error()));
    }
    weight->run_automaton_ = std::make_shared<const RunAutomaton>(std::move(run.value()));

    const bool needs_scores = score_mode_needs_scores(score_mode);
    std::vector<TermStatistics> all_term_stats;
    weight->term_states_.resize(static_cast<size_t>(query.num_term_ids()));
    for (int32_t term_id = 0; term_id < query.num_term_ids(); ++term_id) {
        std::optional<BytesRef> bytes = query.term_bytes(term_id);
        if (!bytes.has_value()) {
            // The wildcard: it has no term of its own, so no postings either.
            continue;
        }
        Term term(query.field(), std::move(bytes.value()));
        auto states = build_term_states(searcher, term, needs_scores);
        if (!states.has_value()) {
            return std::unexpected(std::move(states.error()));
        }
        if (needs_scores) {
            auto doc_freq = states.value().doc_freq();
            if (!doc_freq.has_value()) {
                return std::unexpected(std::move(doc_freq.error()));
            }
            if (doc_freq.value() > 0) {
                auto total_term_freq = states.value().total_term_freq();
                if (!total_term_freq.has_value()) {
                    return std::unexpected(std::move(total_term_freq.error()));
                }
                auto created = searcher.term_statistics(term, doc_freq.value(), total_term_freq.value());
                if (!created.has_value()) {
                    return std::unexpected(std::move(created.error()));
                }
                all_term_stats.push_back(std::move(created.value()));
            }
        }
        weight->term_states_[static_cast<size_t>(term_id)].emplace(std::move(states.value()));
    }

    if (!needs_scores) {
        weight->sim_scorer_ = dummy_sim_scorer();
        return weight;
    }
    if (all_term_stats.empty()) {
        // No term of the automaton occurs in the index, so nothing can match and
        // the similarity is never asked for a score.
        return weight;
    }
    auto collection_stats = searcher.collection_statistics(query.field());
    if (!collection_stats.has_value()) {
        return std::unexpected(std::move(collection_stats.error()));
    }
    if (!collection_stats.value().has_value()) {
        return err(LuceneError::illegal_state("collection statistics are missing"));
    }
    auto scorer = weight->similarity_->scorer(weight->boost_, collection_stats.value().value(), all_term_stats);
    if (!scorer.has_value()) {
        return std::unexpected(std::move(scorer.error()));
    }
    weight->sim_scorer_ = std::move(scorer.value());
    return weight;
}

Result<std::unique_ptr<TermAutomatonScorer>> TermAutomatonWeight::build_scorer(
        const LeafReaderContext &context) const {
    if (run_automaton_ == nullptr || sim_scorer_ == nullptr) {
        return std::unique_ptr<TermAutomatonScorer>{};
    }
    LeafReader *reader = context.reader();
    if (reader == nullptr) {
        return std::unique_ptr<TermAutomatonScorer>{};
    }
    auto field_terms = reader->terms(query_.field());
    if (!field_terms.has_value()) {
        return std::unexpected(std::move(field_terms.error()));
    }
    if (field_terms.value() == nullptr) {
        return std::unique_ptr<TermAutomatonScorer>{};
    }
    auto terms_enum = field_terms.value()->iterator();
    if (!terms_enum.has_value()) {
        return std::unexpected(std::move(terms_enum.error()));
    }

    std::vector<EnumAndScorer> subs;
    subs.reserve(term_states_.size());
    for (size_t term_id = 0; term_id < term_states_.size(); ++term_id) {
        std::optional<TermStates> &states = term_states_[term_id];
        if (!states.has_value()) {
            continue;
        }
        auto prepared = states->get(context);
        if (!prepared.has_value()) {
            return std::unexpected(std::move(prepared.error()));
        }
        std::optional<const TermStateEnum *> state;
        if (prepared.value().has_value()) {
            auto resolved = states->resolve(prepared.value().value());
            if (!resolved.has_value()) {
                return std::unexpected(std::move(resolved.error()));
            }
            state = resolved.value();
        }
        if (!state.has_value()) {
            continue;
        }
        std::optional<BytesRef> bytes = query_.term_bytes(static_cast<int32_t>(term_id));
        if (!bytes.has_value()) {
            return err(LuceneError::illegal_state("term states exist for the ANY term"));
        }
        if (auto seeked = terms_enum.value()->seek_exact_with_state(bytes.value().as_slice(), *state.value());
            !seeked.has_value()) {
            return std::unexpected(std::move(seeked.error()));
        }
        auto postings = terms_enum.value()->postings_with_flags(nullptr, POSITIONS);
        if (!postings.has_value()) {
            return std::unexpected(std::move(postings.error()));
        }
        if (postings.value() == nullptr) {
            continue;
        }
        subs.push_back(EnumAndScorer{.term_id = static_cast<int32_t>(term_id),
                                     .pos_enum = std::move(postings.value())});
    }

    if (subs.empty()) {
        return std::unique_ptr<TermAutomatonScorer>{};
    }

    auto norms = reader->get_norm_values(query_.field());
    if (!norms.has_value()) {
        return std::unexpected(std::move(norms.error()));
    }
    return std::unique_ptr<TermAutomatonScorer>(new TermAutomatonScorer(
            run_automaton_, std::move(subs), query_.any_term_id(), sim_scorer_, std::move(norms.value())));
}

const Query &TermAutomatonWeight::get_query() const { return query_; }

Result<bool> TermAutomatonWeight::is_cacheable(const LeafReaderContext & /*context*/) const { return true; }

Result<std::unique_ptr<ScorerSupplier>> TermAutomatonWeight::scorer_supplier(const LeafReaderContext & /*context*/,
                                                                            const IndexSearcher & /*searcher*/) {
    if (run_automaton_ == nullptr || sim_scorer_ == nullptr) {
        return std::unique_ptr<ScorerSupplier>{};
    }
    return std::unique_ptr<ScorerSupplier>(new TermAutomatonScorerSupplier(*this));
}

Result<Explanation> TermAutomatonWeight::explain(const LeafReaderContext &context, int32_t doc,
                                                const IndexSearcher & /*searcher*/) const {
    auto scorer = build_scorer(context);
    if (!scorer.has_value()) {
        return std::unexpected(std::move(scorer.error()));
    }
    if (scorer.value() == nullptr) {
        return Explanation::no_match_no_details("No matching terms in the document");
    }

    auto advanced = scorer.value()->advance(doc);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    if (advanced.value() != doc) {
        return Explanation::no_match_no_details("No matching terms in the document");
    }

    auto score = scorer.value()->score();
    if (!score.has_value()) {
        return std::unexpected(std::move(score.error()));
    }

    int64_t norm = 1;
    auto norms = context.reader()->get_norm_values(query_.field());
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

    const std::shared_ptr<const SimScorer> &stats = sim_scorer_;
    if (stats == nullptr) {
        return err(LuceneError::illegal_state("similarity scorer is missing"));
    }

    std::vector<Explanation> term_explanations;
    for (const EnumAndScorer &sub : scorer.value()->subs()) {
        if (sub.pos_enum->doc_id() != doc) {
            continue;
        }
        auto frequency = sub.pos_enum->freq();
        if (!frequency.has_value()) {
            return std::unexpected(std::move(frequency.error()));
        }
        const float term_score = stats->score(static_cast<float>(frequency.value()), norm);
        const std::optional<BytesRef> term = query_.term_bytes(sub.term_id);
        const std::string text = term.has_value() ? Term(query_.field(), term.value()).text() : "*";
        term_explanations.push_back(Explanation::match_(
                Number{frequency.value()}, "term frequency in the document",
                {Explanation::match_no_details(Number{term_score}, "score for term: " + text)}));
    }

    if (term_explanations.empty()) {
        return Explanation::no_match_no_details("No matching terms in the document");
    }

    Explanation freq_explanation =
            Explanation::match_(Number{score.value()}, "TermAutomatonQuery, sum of:", std::move(term_explanations));
    return stats->explain(freq_explanation, norm);
}

} // namespace lucene
