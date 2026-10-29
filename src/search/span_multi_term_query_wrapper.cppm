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

export module lucene_core:span_multi_term_query_wrapper;

import :index_searcher;
import :multi_term_query;
import :query;
import :query_visitor;
import :score_mode;
import :span_query;
import :types;
import :weight;

import std;

namespace lucene {

/// Presents a [`MultiTermQuery`](lucene_core:multi_term_query) (prefix, wildcard,
/// regexp, fuzzy, term range, …) as a [`SpanQuery`], so it can be nested in span
/// queries such as [`SpanFirstQuery`](lucene_core:span_first_query).
///
/// Port of `SpanMultiTermQueryWrapper`
/// (`queries/span/span_multi_term_query_wrapper.rs`).  [`rewrite`] expands the
/// wrapped query into a [`SpanOrQuery`](lucene_core:span_or_query) of
/// [`SpanTermQuery`](lucene_core:span_term_query)s, which is the only way a span
/// can carry positions for more than one term.
///
/// **Deviations:**
///
/// 1. The wrapper cannot be weighted directly — like the reference
///    implementation, [`create_weight`] reports `illegal_argument`, so an index
///    searcher has to rewrite the query first (which `IndexSearcher::search` does).
/// 2. The expansion always keeps **all** terms, even when the wrapped query uses a
///    top-terms rewrite: a `SpanOrQuery` has no clause limit, so the cap the
///    reference implementation copies from the wrapped rewrite method would only
///    change how many spans are built, not which documents match.
/// 3. An expansion without any term becomes `MatchNoDocsQuery`; the reference
///    implementation would build an empty `SpanOrQuery`, which has no field.
export class SpanMultiTermQueryWrapper final : public SpanQuery {
public:
    explicit SpanMultiTermQueryWrapper(std::shared_ptr<MultiTermQuery> query);

    [[nodiscard]] const std::shared_ptr<MultiTermQuery> &wrapped_query() const noexcept { return query_; }

    [[nodiscard]] std::string_view get_field() const override { return query_->field(); }

    /// Always fails: the wrapper has to be rewritten before it can be weighted.
    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                               float boost) const override;

    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    std::shared_ptr<MultiTermQuery> query_;
};

} // namespace lucene
