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

export module lucene_core:query_builder;

import :analyzer;
import :boolean_clause;
import :error;
import :occur;
import :query;
import :term;

import std;

namespace lucene {

/// One term produced by the analysis chain, with the boost the chain attached
/// to it.
///
/// **Deviation:** the analysis chain of this port has no `BoostAttribute`, so
/// [`QueryBuilder`] always reports [`QueryBuilder::DEFAULT_BOOST`] here; the
/// struct keeps the field so that a boosting filter can be added later without
/// changing the hook signatures.  The term itself is kept as the raw UTF-8
/// bytes of the token, exactly what the reference implementation's `BytesRef`
/// holds.
export struct TermAndBoost {
    std::string text;
    float boost{1.0F};
};

/// Builds queries from an [`Analyzer`] chain.
///
/// Port of `QueryBuilder` (`core/util/query_builder.rs`).  Given a piece of
/// user text, the analyzer decides how many terms it turns into and at which
/// positions, and this class turns that into a `TermQuery`, `SynonymQuery`,
/// `PhraseQuery`, `MultiPhraseQuery` or `BooleanQuery`:
///
/// * a single token becomes a term query;
/// * several tokens at distinct positions become a boolean query (`MUST` for
///   phrase queries, the requested operator otherwise);
/// * tokens sharing one position (a synonym) become a synonym query, or a
///   multi-phrase when the query is quoted;
/// * a quoted text with more than one position becomes a phrase query.
///
/// **Deviations** from the reference implementation:
///
/// 1. Graph queries are not available: `GraphTokenStreamFiniteStrings` and
///    `TokenStreamToAutomaton` are not ported yet, so a token stream with
///    `positionLength > 1` (a multi-word synonym graph) is treated as if every
///    token spanned a single position.  Single-token synonyms — the common
///    case, produced by `positionIncrement == 0` — are handled exactly like the
///    reference implementation.
/// 2. `BoostAttribute` does not exist in the analysis chain, so every term is
///    produced with [`DEFAULT_BOOST`] (there is no per-token boost to honour).
/// 3. The hooks return `Result` so that a `BooleanQuery` that exceeds
///    `max_clause_count` reports an error instead of throwing.
export class QueryBuilder {
public:
    /// The boost a term gets when nothing else applies.
    static constexpr float DEFAULT_BOOST = 1.0F;

    explicit QueryBuilder(Analyzer &analyzer);

    virtual ~QueryBuilder() = default;

    QueryBuilder(const QueryBuilder &) = delete;
    QueryBuilder &operator=(const QueryBuilder &) = delete;
    QueryBuilder(QueryBuilder &&) = delete;
    QueryBuilder &operator=(QueryBuilder &&) = delete;

    [[nodiscard]] Analyzer &analyzer() const noexcept { return *analyzer_; }

    /// Whether a token that skips positions (`positionIncrement > 1`) keeps those
    /// gaps in the produced phrase.  Defaults to `true`.
    void set_enable_position_increments(bool value) noexcept { enable_position_increments_ = value; }
    [[nodiscard]] bool enable_position_increments() const noexcept { return enable_position_increments_; }

    /// Whether a multi-word synonym is turned into a phrase query.  Kept for API
    /// compatibility; graph queries are not ported yet (see the class notes), so
    /// nothing reads it today.
    void set_auto_generate_multi_term_synonyms_phrase_query(bool value) noexcept {
        auto_generate_multi_term_synonyms_phrase_query_ = value;
    }
    [[nodiscard]] bool auto_generate_multi_term_synonyms_phrase_query() const noexcept {
        return auto_generate_multi_term_synonyms_phrase_query_;
    }

    /// [`create_boolean_query`] with `Occur::Should`.
    [[nodiscard]] Result<std::shared_ptr<Query>> create_boolean_query(std::string_view field,
                                                                    std::string_view query_text);

    /// Analyzes `query_text` and joins the terms with `occur`, which must be
    /// `Occur::Should` or `Occur::Must`.
    [[nodiscard]] Result<std::shared_ptr<Query>> create_boolean_query(std::string_view field, std::string_view query_text,
                                                                    Occur occur);

    /// Analyzes `query_text` into a phrase query with `phrase_slop`; a single
    /// token still produces a term query.
    [[nodiscard]] Result<std::shared_ptr<Query>> create_phrase_query(std::string_view field, std::string_view query_text,
                                                                    int32_t phrase_slop = 0);

    /// The general entry point: `quoted` asks for phrases whenever the analysis
    /// produced more than one position.
    ///
    /// Returns a null pointer when the analysis produced no token at all (an
    /// empty query, or one that only held stop words), which callers are
    /// expected to turn into "no query" rather than into a match-all.
    [[nodiscard]] Result<std::shared_ptr<Query>> create_field_query(Occur occur, std::string_view field,
                                                                   std::string_view query_text, bool quoted,
                                                                   int32_t phrase_slop);

protected:
    // ---- hooks: subclasses build the concrete query objects ----

    /// A term query, boosted when `boost` differs from [`DEFAULT_BOOST`].
    [[nodiscard]] virtual Result<std::shared_ptr<Query>> new_term_query(const Term &term, float boost);

    /// Terms that sit at one position.  A single term collapses to a term query.
    [[nodiscard]] virtual Result<std::shared_ptr<Query>> new_synonym_query(std::string_view field,
                                                                          std::span<const TermAndBoost> terms);

    /// A boolean query out of `clauses`.
    [[nodiscard]] virtual Result<std::shared_ptr<Query>> new_boolean_query(std::vector<BooleanClause> clauses);

    /// A phrase query; `positions` may contain gaps when position increments are
    /// enabled.
    [[nodiscard]] virtual Result<std::shared_ptr<Query>> new_phrase_query(std::string field, std::vector<Term> terms,
                                                                        std::vector<int32_t> positions, int32_t slop);

    /// A phrase query where more than one term may sit at a position.
    [[nodiscard]] virtual Result<std::shared_ptr<Query>> new_multi_phrase_query(
        std::vector<std::vector<Term>> terms_at_positions, std::vector<int32_t> positions, int32_t slop);

private:
    Analyzer *analyzer_;
    bool enable_position_increments_{true};
    bool auto_generate_multi_term_synonyms_phrase_query_{false};
};

} // namespace lucene
