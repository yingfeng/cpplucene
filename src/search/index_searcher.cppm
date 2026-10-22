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

export module lucene_core:index_searcher;

import :types;
import std;

import :bulk_scorer;
import :collector;
import :collector_manager;
import :collection_statistics;
import :error;
import :identity;
import :index_reader;
import :leaf_reader;
import :leaf_reader_context;
import :query;
import :score_doc;
import :score_mode;
import :scorer_supplier;
import :similarity;
import :term;
import :term_statistics;
import :top_docs;
import :weight;

namespace lucene {

/// Runs queries against a [`IndexReader`].
///
/// Port of `IndexSearcher` (`core/search/index_searcher.rs`).
///
/// This is the reader/statistics half of the class: it owns the reader and the
/// similarity, and turns field/term information into the
/// [`CollectionStatistics`] / [`TermStatistics`] a `Weight` needs.
///
/// **Deviations** (C++ mechanics only, all of them "not ported yet"):
///  * searching is serial: slices, partitions and the task executor are not
///    ported, so one collector is created per leaf and the leaves are visited in
///    order;
///  * `count` and `explain` are not ported yet, and neither are the query cache,
///    caching policy, timeouts or the `IndexSearcherHook`;
///  * the reader is a `std::shared_ptr` instead of a ref-counted
///    `IndexReaderContext` tree.
export class IndexSearcher {
public:
    /// A searcher over `reader`; a null `similarity` means BM25 with the default
    /// parameters.
    explicit IndexSearcher(std::shared_ptr<IndexReader> reader, std::shared_ptr<Similarity> similarity = nullptr);

    virtual ~IndexSearcher() = default;

    /// The reader being searched.
    [[nodiscard]] IndexReader &reader() const noexcept { return *reader_; }

    /// The atomic leaves of the reader, in document order.
    [[nodiscard]] Result<std::span<const LeafReaderContext>> leaves() const { return reader_->leaves(); }

    /// Identity of the top-level reader; what a `Weight` compares against the
    /// `TermStates` it built.
    [[nodiscard]] const Identity &top_reader_identity() const noexcept { return reader_->identity(); }

    /// The similarity used to score.
    [[nodiscard]] const Similarity &similarity() const noexcept { return *similarity_; }

    /// The similarity, as a shared pointer: a `Weight` outlives the call that
    /// created it, so it keeps the similarity alive (rlucene clones the `Arc`).
    [[nodiscard]] const std::shared_ptr<Similarity> &similarity_ptr() const noexcept { return similarity_; }

    /// Builds the searcher-dependent state of `query`.
    ///
    /// Port of `IndexSearcher::create_weight`.  **Deviation:** the query cache
    /// hook is not ported, so this is a plain forward to
    /// [`Query::create_weight`].
    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const Query &query, ScoreMode score_mode,
                                                                float boost) const {
        return query.create_weight(*this, score_mode, boost);
    }

    /// Statistics of `field` aggregated over all leaves, or `std::nullopt` when
    /// no document has a term for it.
    [[nodiscard]] Result<std::optional<CollectionStatistics>> collection_statistics(std::string_view field) const;

    /// Statistics of one term.  Kept as a method (like rlucene) so that a
    /// subclass can answer from a distributed collection.
    [[nodiscard]] Result<TermStatistics> term_statistics(const Term &term, int32_t doc_freq,
                                                         int64_t total_term_freq) const;

    /// Documents containing `term`, over all leaves.
    [[nodiscard]] Result<int32_t> doc_freq(const Term &term) const;

    /// Occurrences of `term` over all leaves.
    [[nodiscard]] Result<int64_t> total_term_freq(const Term &term) const;

    // ---- searching ----

    /// How many hits are counted exactly by default; above this the reported
    /// total becomes a lower bound.  rlucene's `TOTAL_HITS_THRESHOLD`.
    static constexpr size_t TOTAL_HITS_THRESHOLD = 1000;

    /// The top `n` hits for `query`, sorted by score.
    [[nodiscard]] Result<TopDocs> search(const Query &query, int32_t n) const;

    /// The top hits at or after the `after` cursor; `after` may be null, which
    /// makes this the first page.
    [[nodiscard]] Result<TopDocs> search_after_score(const ScoreDoc *after, const Query &query, int32_t num_hits) const;

    /// Runs `query` through a collector manager (one collector per leaf) and
    /// reduces the results.
    [[nodiscard]] Result<TopDocs> search_with_collector_manager(const Query &query,
                                                                const CollectorManager &manager) const;

    /// Feeds every leaf into `collector`; the collector keeps the results.
    [[nodiscard]] VoidResult search_with_collector(const Query &query, Collector &collector) const;

    /// Rewrites `query` until it reaches its final form.
    ///
    /// **Deviation:** rlucene's `rewrite_with_needs_scores` wraps the query in a
    /// `ConstantScoreQuery` when scores are not needed, to take advantage of its
    /// extra rewrite rules; that query is not ported, so this always rewrites
    /// directly.
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const Query &query) const;

private:
    /// Collects the leaf at `ord`, restricted to the document range
    /// `[min_doc_id, max_doc_id)`.
    [[nodiscard]] VoidResult search_leaf(int32_t ord, int32_t min_doc_id, int32_t max_doc_id, Weight &weight,
                                         Collector &collector) const;

    std::shared_ptr<IndexReader> reader_;
    std::shared_ptr<Similarity> similarity_;
};

} // namespace lucene
