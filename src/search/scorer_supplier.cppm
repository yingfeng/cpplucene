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

export module lucene_core:scorer_supplier;

import :types;
import std;

import :bulk_scorer;
import :error;
import :leaf_reader_context;
import :scorer;

namespace lucene {

export class IndexSearcher;

/// Supplies a [`Scorer`], so its cost can be known before it is built.
///
/// Port of the `ScorerSupplier` trait (`core/search/scorer_supplier.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the associated `Scorer` / `BulkScorer` types collapse into
///    `std::unique_ptr<Scorer>` / `std::unique_ptr<BulkScorer>`; a null pointer
///    means "no bulk scorer";
///  * [`bulk_scorer`] defaults to wrapping `get(INT64_MAX)` in a
///    [`DefaultBulkScorer`], which is what rlucene's `default_bulk_scorer` does
///    (rlucene keeps both because the default one is also callable explicitly).
export class ScorerSupplier {
public:
    virtual ~ScorerSupplier() = default;

    /// Builds the scorer.  Must be called at most once.
    ///
    /// `lead_cost` is the cost of the scorer that will lead iteration, i.e. an
    /// upper bound on the number of `next_doc` / `advance` / `matches` calls;
    /// pass `INT64_MAX` when in doubt.
    [[nodiscard]] virtual Result<std::unique_ptr<Scorer>> get(int64_t lead_cost, const LeafReaderContext &context,
                                                              const IndexSearcher &searcher) = 0;

    /// An estimate of the cost of [`get`].
    [[nodiscard]] virtual Result<int64_t> cost(const LeafReaderContext &context, const IndexSearcher &searcher) = 0;

    /// A bulk scorer; the default wraps `get(INT64_MAX)` in a
    /// [`DefaultBulkScorer`], which iterates the scorer and feeds a collector.
    [[nodiscard]] virtual Result<std::unique_ptr<BulkScorer>> bulk_scorer(const LeafReaderContext &context,
                                                                         const IndexSearcher &searcher);

    /// Tells the supplier that its scores go straight to the collector instead
    /// of being combined (e.g. summed).
    [[nodiscard]] virtual VoidResult set_top_level_scoring_clause() { return {}; }
};

} // namespace lucene
