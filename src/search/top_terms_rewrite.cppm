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

export module lucene_core:top_terms_rewrite;

import :types;
import std;

import :error;
import :index_searcher;
import :multi_term_query;
import :query;

namespace lucene {

/// How [`rewrite_top_terms`] turns the collected terms into a query.
///
/// The reference implementation expresses the three top-terms rewrites as three
/// `RewriteMethod` structs; the port keeps the shared collection and switches on
/// this value when it builds the query.
export enum class TopTermsBuildMode : uint8_t {
    /// A boolean query of `BoostQuery(TermQuery)`: the reference implementation's
    /// `TopTermsScoringBooleanQueryRewrite`.
    kScoringBoolean = 0,
    /// A boolean query of `BoostQuery(ConstantScoreQuery(TermQuery))`: its
    /// `TopTermsBoostOnlyBooleanQueryRewrite`.
    kBoostOnlyBoolean = 1,
    /// A [`BlendedTermQuery`](:blended_term_query) whose terms share the blended
    /// statistics: its `TopTermsBlendedFreqScoringRewrite`.
    kBlendedFreq = 2,
};

/// Keeps the `size` best-boosted terms of `query` (at most
/// `BooleanQuery::max_clause_count()` of them) and turns them into a query.
///
/// Port of `TopTermsRewrite::default_rewrite` and `TermCollectorImpl`
/// (`core/search/top_terms_rewrite.rs`).  Boosts come from the
/// `BoostAttribute` a terms enum publishes; when it publishes none, every term
/// is weighted `1.0` and the lexicographically smallest terms are kept, which is
/// what the reference implementation does as well.
export [[nodiscard]] Result<std::shared_ptr<Query>>
rewrite_top_terms(const IndexSearcher &searcher, const MultiTermQuery &query, size_t size, TopTermsBuildMode mode);

} // namespace lucene
