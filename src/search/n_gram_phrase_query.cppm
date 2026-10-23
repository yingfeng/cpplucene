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

export module lucene_core:n_gram_phrase_query;

import :types;
import std;

import :error;
import :index_searcher;
import :phrase_query;
import :query;
import :query_visitor;
import :score_mode;
import :term;
import :weight;

namespace lucene {

/// A [`PhraseQuery`](:phrase_query) optimized for n-gram fields.
///
/// Port of `NGramPhraseQuery` (`core/search/n_gram_phrase_query.rs`).
///
/// Querying `"ABCD"` on a bigram field with a plain phrase query asks for
/// `AB/0 BC/1 CD/2`, where `BC` is implied by `AB` and `CD`; this query rewrites
/// itself to `AB/0 CD/2` and therefore looks at fewer positions.
export class NGramPhraseQuery final : public Query {
public:
    /// A query of n-gram size `n` over `phrase_query`.
    NGramPhraseQuery(size_t n, PhraseQuery phrase_query);

    /// The n of the n-gram.
    [[nodiscard]] size_t n() const noexcept { return n_; }
    /// The terms of the wrapped phrase query.
    [[nodiscard]] const std::vector<Term> &terms() const noexcept { return phrase_query_.terms(); }
    /// The positions of the wrapped phrase query.
    [[nodiscard]] const std::vector<int32_t> &positions() const noexcept { return phrase_query_.positions(); }
    /// The wrapped phrase query.
    [[nodiscard]] const PhraseQuery &phrase_query() const noexcept { return phrase_query_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    /// Drops the interior terms of every n-gram when the phrase has no slop, at
    /// least three terms and consecutive positions; otherwise rewrites the
    /// wrapped phrase query.
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    /// **Deviation:** the reference implementation visits through a sub visitor
    /// for the `MUST` occurrence; the port has no `get_sub_visitor` yet.
    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// The text of the wrapped phrase query.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    size_t n_;
    PhraseQuery phrase_query_;
};

} // namespace lucene
