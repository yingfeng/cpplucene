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

export module lucene_core:term_automaton_query;

import :types;
import std;

import :automaton;
import :bytes_ref;
import :error;
import :explanation;
import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :run_automaton;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :similarity;
import :term;
import :term_automaton_scorer;
import :term_states;
import :weight;

namespace lucene {

export class TermAutomatonQuery;

/// Scores a [`TermAutomatonQuery`]: one [`TermAutomatonScorer`] per leaf, built
/// from the postings of every term the automaton mentions.
///
/// Port of `TermAutomatonWeight` (`core/search/term_automaton_query.rs`).
export class TermAutomatonWeight final : public Weight {
public:
    /// Builds the weight of `query`, reading the statistics the similarity needs
    /// and compiling the automaton into a [`RunAutomaton`].
    ///
    /// `query` must already be finished; it must also outlive the weight, which
    /// only borrows it.
    [[nodiscard]] static Result<std::unique_ptr<TermAutomatonWeight>> create(const TermAutomatonQuery &query, float boost,
                                                                            ScoreMode score_mode,
                                                                            const IndexSearcher &searcher);

    // ---- Weight ----

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override;

    [[nodiscard]] const Query &get_query() const override;

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher &searcher) override;

    /// The weight only depends on the reader statistics, so it is cacheable.
    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override;

    /// Builds the scorer of one leaf, or a null pointer when no term of the
    /// automaton occurs there.  Public because the scorer supplier of this
    /// weight delegates to it.
    [[nodiscard]] Result<std::unique_ptr<TermAutomatonScorer>> build_scorer(const LeafReaderContext &context) const;

private:
    TermAutomatonWeight(const TermAutomatonQuery &query, float boost, ScoreMode score_mode,
                        std::shared_ptr<const Similarity> similarity);

    const TermAutomatonQuery &query_;
    float boost_;
    ScoreMode score_mode_;
    std::shared_ptr<const Similarity> similarity_;
    /// Null when no term of the automaton occurs in the index, i.e. the query
    /// cannot match anything.
    std::shared_ptr<const SimScorer> sim_scorer_;
    /// One entry per term id; the wildcard id has no term and stays empty.  The
    /// states resolve their postings lazily, which is why the vector is mutable
    /// in a `const` weight.
    mutable std::vector<std::optional<TermStates>> term_states_;
    /// The determinized automaton, compiled once and shared by every leaf.
    std::shared_ptr<const RunAutomaton> run_automaton_;
};

/// A proximity query that matches documents where an automaton whose transitions
/// are terms matches.
///
/// Port of `TermAutomatonQuery` (`core/search/term_automaton_query.rs`).
///
/// This generalizes [`PhraseQuery`](lucene_core:phrase_query) and
/// `SpanNearQuery`: after [`create_state`], [`set_accept`], [`add_transition`]
/// and [`add_any_transition`] built the automaton, [`finish`] compiles it and
/// the query can be run.  It is likely slow, since it visits any document
/// containing any of its terms (a disjunction, not a conjunction) and then
/// merge-sorts the positions of each document to count the matches.
///
/// Experimental: this API follows the reference implementation's experimental
/// status.
///
/// **Deviations** (C++ mechanics only):
///  * terms are keyed by their bytes: [`add_transition`] takes text, which is
///    UTF-8 encoded, and [`add_transition_bytes`] takes the raw bytes;
///  * the reference implementation derives `PartialEq`/`Hash` from the query's
///    identity token; here [`equals`] and [`hash`] do the same through
///    [`Query::identity`].
export class TermAutomatonQuery final : public Query {
public:
    /// A query over `field` with a single automaton state (the initial one) and
    /// no transitions.
    explicit TermAutomatonQuery(std::string field);

    /// Returns a new state.  A fresh query has no state at all, so the first call
    /// returns the initial state `0`.
    [[nodiscard]] int32_t create_state();

    /// Marks `state` as accepting or not.
    void set_accept(int32_t state, bool accept);

    /// Adds a transition from `source` to `dest` labeled with `term`.
    ///
    /// A `*` is an ordinary term here: it is the token stream wrapper that maps
    /// it to [`add_any_transition`].
    [[nodiscard]] VoidResult add_transition(int32_t source, int32_t dest, std::string_view term);

    /// [`add_transition`] with the term's raw bytes.
    [[nodiscard]] VoidResult add_transition_bytes(int32_t source, int32_t dest, std::span<const uint8_t> term);

    /// Adds a transition matching any term.
    [[nodiscard]] VoidResult add_any_transition(int32_t source, int32_t dest);

    /// Compiles the automaton; call once the states and transitions are added.
    [[nodiscard]] VoidResult finish();

    /// [`finish`] with an explicit determinization effort limit.
    [[nodiscard]] VoidResult finish_with_work_limit(size_t determinize_work_limit);

    /// The Graphviz (dot) rendering of the compiled automaton.
    [[nodiscard]] Result<std::string> to_dot() const;

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    /// The term id of the wildcard transition, or `-1` when the automaton has
    /// none.
    [[nodiscard]] int32_t any_term_id() const noexcept { return any_term_id_; }
    /// How many term ids exist, wildcard included; also the automaton's alphabet
    /// size.
    [[nodiscard]] int32_t num_term_ids() const noexcept { return static_cast<int32_t>(id_to_term_.size()); }
    /// Whether [`finish`] was called.
    [[nodiscard]] bool is_finished() const noexcept { return determinized_.has_value(); }
    /// The compiled automaton; [`finish`] must have been called.
    [[nodiscard]] const Automaton &determinized() const;
    /// The bytes of `term_id`, or `nullopt` for the wildcard.
    [[nodiscard]] std::optional<BytesRef> term_bytes(int32_t term_id) const;

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    /// An automaton accepting nothing becomes `MatchNoDocsQuery`, one accepting a
    /// single term a `TermQuery`, and a "sausage" becomes a `PhraseQuery` or a
    /// `MultiPhraseQuery`.
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `TermAutomatonQuery(field=<field> numStates=<n>)`.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    /// Identity equality, like the reference implementation's derived `PartialEq`.
    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    /// The term id of `term`, creating it when needed; `nullopt` addresses the
    /// wildcard.
    [[nodiscard]] Result<int32_t> get_term_id(std::optional<std::span<const uint8_t>> term);

    [[nodiscard]] const Automaton &require_determinized() const;

    std::string field_;
    AutomatonBuilder builder_;
    std::optional<Automaton> determinized_;
    /// Term bytes to id; the bytes are kept as a `std::string` so that they can
    /// be looked up without copying.
    std::unordered_map<std::string, int32_t> term_to_id_;
    /// Id to term bytes; the wildcard slot is empty.
    std::vector<std::optional<std::string>> id_to_term_;
    int32_t any_term_id_{-1};
};

} // namespace lucene
