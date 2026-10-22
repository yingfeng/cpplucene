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

export module lucene_core:weight;

import :types;
import std;

import :bulk_scorer;
import :error;
import :explanation;
import :leaf_reader_context;
import :score_mode;
import :scorable;
import :scorer;
import :scorer_supplier;
import :segment_cacheable;

namespace lucene {

export class IndexSearcher;
export class Query;

/// Expert: computes query weights and builds query scorers.
///
/// Port of the `Weight` trait (`core/search/weight.rs`).
///
/// A [`Weight`] keeps the searcher-dependent state of a query, so that the query
/// itself is never modified and can be reused.  It is created once per search
/// (see `Query::create_weight`) and then asked for a scorer or a bulk scorer per
/// leaf.
///
/// **Deviations** (C++ mechanics only):
///  * `matches` / `QueryWeightMatches` are not ported yet: they need the
///    `Matches` / `MatchesIterator` machinery, which no ported query needs so
///    far;
///  * `get_query` returns a borrowed reference: the reference implementation's `create_weight`
///    consumes the query into an `Arc`, so the weight owns it, while here the
///    caller keeps the query alive for as long as the weight is used (the same
///    contract Java's `Weight` has);
///  * `into_inner_weight` and `is_cache_wrapper` (query-cache plumbing) are not
///    ported.
export class Weight : public SegmentCacheable {
public:
    ~Weight() override = default;

    /// Explains the score of `doc`.
    [[nodiscard]] virtual Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                                      const IndexSearcher &searcher) const = 0;

    /// The query this weight belongs to.
    [[nodiscard]] virtual const Query &get_query() const = 0;

    /// A scorer over the leaf `context` describes, or a null pointer when no
    /// document of that leaf matches.
    ///
    /// The default goes through [`scorer_supplier`].
    [[nodiscard]] virtual Result<std::unique_ptr<Scorer>> scorer(const LeafReaderContext &context,
                                                                 const IndexSearcher &searcher);

    /// A supplier of the scorer, so its cost can be inspected first.
    [[nodiscard]] virtual Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                                  const IndexSearcher &searcher) = 0;

    /// A bulk scorer over the leaf `context` describes; the default goes through
    /// [`scorer_supplier`] marked as top-level scoring clause.
    [[nodiscard]] virtual Result<std::unique_ptr<BulkScorer>> bulk_scorer(const LeafReaderContext &context,
                                                                          const IndexSearcher &searcher);

    /// Number of matching live documents in the leaf, or `-1` when it cannot be
    /// computed in sub-linear time.
    [[nodiscard]] virtual Result<int32_t> count(const LeafReaderContext &context, const IndexSearcher &searcher) {
        (void)searcher;
        return default_count(context);
    }

    /// The reference implementation's `default_count`: the "unknown" answer.
    [[nodiscard]] virtual Result<int32_t> default_count(const LeafReaderContext &context) {
        (void)context;
        return -1;
    }
};

} // namespace lucene
