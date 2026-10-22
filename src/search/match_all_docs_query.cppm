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

export module lucene_core:match_all_docs_query;

import :types;

import std;

import :error;
import :query;
import :score_mode;
import :weight;

namespace lucene {

export class IndexSearcher;

/// A query that matches every document with a constant score.
///
/// Port of `MatchAllDocsQuery` (`core/search/match_all_docs_query.rs`).
///
/// **Deviation:** the reference implementation has a dedicated `MatchAllBulkScorer` that walks the
/// document range without a scorer; the port uses a
/// [`ConstantScoreScorer`](lucene_core:constant_score_scorer) over an
/// all-documents iterator, which gives the same results through
/// `DefaultBulkScorer` and keeps one code path.
export class MatchAllDocsQuery final : public Query {
public:
    MatchAllDocsQuery();

    [[nodiscard]] static std::shared_ptr<MatchAllDocsQuery> create();

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;
};

} // namespace lucene
