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
import :query_timeout;
import :score_doc;
import :score_mode;
import :scorer_supplier;
import :similarity;
import :term;
import :top_field_collector;
import :top_field_collector_manager;
import :sort_field;
import :leaf_slice;
import :task_executor;
import :top_field_docs;
import :total_hit_count_collector;
import :term_statistics;
import :top_docs;
import :weight;

namespace lucene {

export class QueryCache;
export class QueryCachingPolicy;

/// Runs queries against a [`IndexReader`].
///
/// Port of `IndexSearcher` (`core/search/index_searcher.rs`).
///
/// This is the reader/statistics half of the class: it owns the reader and the
/// similarity, and turns field/term information into the
/// [`CollectionStatistics`] / [`TermStatistics`] a `Weight` needs.
///
/// **Deviations** (C++ mechanics only, all of them "not ported yet"):
///  * slices group whole segments: the reference implementation can also split a
///    single huge segment into partitions, and it caches the slices it computed;
///    the port computes whole-segment slices per search, so a searcher can be
///    used from several threads;
///  * without a task executor the leaves are visited one by one, in order; a
///    searcher built with [`with_executor`] runs one task per leaf slice, which
///    is safe because a leaf belongs to exactly one slice (a weight must
///    therefore be usable for one leaf at a time, which is what the reference
///    implementation's `Send + Sync` bounds enforce as well);
///  * a search that uses the query cache runs its slices one after another: the
///    port's cache is not synchronized;
///  * the `IndexSearcherHook` is not ported; the query cache is only consulted
///    when a cache and a policy are set, and only for searches that do not need
///    scores.  Timeouts are supported through the [`QueryTimeout`] overloads of
///    `search` / `search_after_score`, but there is no `IndexSearcher` level
///    executor that would abort a search without one;
///  * the reader is a `std::shared_ptr` instead of a ref-counted
///    `IndexReaderContext` tree.
export class IndexSearcher {
public:
    /// A searcher over `reader`; a null `similarity` means BM25 with the default
    /// parameters, and a null `task_executor` searches every slice on the calling
    /// thread.
    explicit IndexSearcher(std::shared_ptr<IndexReader> reader, std::shared_ptr<Similarity> similarity = nullptr,
                           std::shared_ptr<TaskExecutor> task_executor = nullptr);

    /// A searcher that runs the leaf slices of a search on `threads` workers.
    ///
    /// Port of `IndexSearcher::with_executor`.
    [[nodiscard]] static Result<std::unique_ptr<IndexSearcher>>
    with_executor(std::shared_ptr<IndexReader> reader, std::shared_ptr<Similarity> similarity, size_t threads);

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

    /// The executor a collector manager search splits its slices over.  A search
    /// that uses the query cache ignores it (the cache is not synchronized).
    [[nodiscard]] const TaskExecutor &task_executor() const noexcept { return *task_executor_; }

    /// The similarity, as a shared pointer: a `Weight` outlives the call that
    /// created it, so it keeps the similarity alive (the reference implementation clones the `Arc`).
    [[nodiscard]] const std::shared_ptr<Similarity> &similarity_ptr() const noexcept { return similarity_; }

    /// Builds the searcher-dependent state of `query`.
    ///
    /// Port of `IndexSearcher::create_weight`: when a query cache is set and the
    /// search does not need scores, the weight is wrapped so that the matching
    /// documents are cached per leaf.
    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const Query &query, ScoreMode score_mode,
                                                                float boost) const;

    /// Why `doc` scores what it scores for `query`.
    ///
    /// Port of `IndexSearcher::explain`: the query is rewritten, its weight is
    /// built, and the leaf that holds `doc` explains the (leaf-relative) document.
    /// A document outside the index fails with `illegal_argument`; a document that
    /// does not match answers a non-matching [`Explanation`].
    [[nodiscard]] Result<Explanation> explain(const Query &query, int32_t doc) const;

    /// [`explain`](Self::explain) for a hit, which carries the global document id.
    [[nodiscard]] Result<Explanation> explain(const Query &query, const ScoreDoc &score_doc) const;

    /// A query cache, or a null pointer (no caching).
    [[nodiscard]] const std::shared_ptr<QueryCache> &query_cache() const noexcept { return query_cache_; }

    /// Sets the query cache; a null pointer disables caching.
    void set_query_cache(std::shared_ptr<QueryCache> cache) noexcept { query_cache_ = std::move(cache); }

    /// The policy that decides which queries the cache keeps, `nullptr` when no
    /// cache is set.
    [[nodiscard]] const std::shared_ptr<const QueryCachingPolicy> &caching_policy() const noexcept {
        return caching_policy_;
    }

    /// Sets the caching policy; without one, a cache is never consulted.
    void set_caching_policy(std::shared_ptr<const QueryCachingPolicy> policy) noexcept {
        caching_policy_ = std::move(policy);
    }

    /// Statistics of `field` aggregated over all leaves, or `std::nullopt` when
    /// no document has a term for it.
    [[nodiscard]] Result<std::optional<CollectionStatistics>> collection_statistics(std::string_view field) const;

    /// Statistics of one term.  Kept as a method (like the reference implementation) so that a
    /// subclass can answer from a distributed collection.
    [[nodiscard]] Result<TermStatistics> term_statistics(const Term &term, int32_t doc_freq,
                                                         int64_t total_term_freq) const;

    /// Documents containing `term`, over all leaves.
    [[nodiscard]] Result<int32_t> doc_freq(const Term &term) const;

    /// Occurrences of `term` over all leaves.
    [[nodiscard]] Result<int64_t> total_term_freq(const Term &term) const;

    // ---- searching ----

    /// How many hits are counted exactly by default; above this the reported
    /// total becomes a lower bound.  The reference implementation's `TOTAL_HITS_THRESHOLD`.
    static constexpr size_t TOTAL_HITS_THRESHOLD = 1000;

    /// The process-wide limit on the number of `BooleanQuery` clauses.
    ///
    /// Port of `IndexSearcher.getMaxClauseCount`; see
    /// [`BooleanQuery::max_clause_count`](:boolean_query).
    [[nodiscard]] static size_t get_max_clause_count() noexcept;

    /// Changes the clause limit; `count` must be positive.
    ///
    /// Port of `IndexSearcher.setMaxClauseCount`.
    [[nodiscard]] static VoidResult set_max_clause_count(size_t count);

    /// The top `n` hits for `query`, sorted by score.
    [[nodiscard]] Result<TopDocs> search(const Query &query, int32_t n) const;

    /// The top `n` hits for `query`, giving up when `timeout` fires.
    ///
    /// Port of `IndexSearcher::search(Query, int, QueryTimeout)`: the timeout
    /// surfaces as `time_exceeded` instead of a truncated page, so a caller can
    /// tell "no results" from "stopped early".
    [[nodiscard]] Result<TopDocs> search(const Query &query, int32_t n, const QueryTimeout &timeout) const;

    /// How many documents match `query`.
    ///
    /// Port of `IndexSearcher::count`: it runs a [`TotalHitCountCollector`](:total_hit_count_collector),
    /// which skips the leaves the weight can count without visiting them.
    [[nodiscard]] Result<int32_t> count(const Query &query) const;

    /// The `num_hits` best hits ordered by `sort`.
    ///
    /// Port of `IndexSearcher::search(sort)`: it runs a
    /// [`TopFieldCollectorManager`](:top_field_collector_manager), so a sorted
    /// search is split over the leaf slices like a relevance search and merged by
    /// sort value.  **Deviation:** there is no minimum-competitive-score path, so
    /// the total-hits threshold only makes the count a lower bound.
    [[nodiscard]] Result<TopFieldDocs> search(const Query &query, const Sort &sort, int32_t n) const;

    /// The `num_hits` best hits ordered by `sort` that come after `after`; a null
    /// `after` collects the first page.  Pass the last hit of a page (its
    /// [`FieldDoc`](:top_field_docs) values) to get the next one, which is how
    /// deep paging works with a sort.
    ///
    /// Port of `IndexSearcher::search_after(FieldDoc, Query, int, Sort)`.
    [[nodiscard]] Result<TopFieldDocs> search_after(const FieldDoc *after, const Query &query, const Sort &sort,
                                                    int32_t num_hits) const;

    /// The top hits at or after the `after` cursor; `after` may be null, which
    /// makes this the first page.
    [[nodiscard]] Result<TopDocs> search_after_score(const ScoreDoc *after, const Query &query, int32_t num_hits) const;

    /// [`search_after_score`] with a timeout; see [`search`].
    [[nodiscard]] Result<TopDocs> search_after_score(const ScoreDoc *after, const Query &query, int32_t num_hits,
                                                     const QueryTimeout &timeout) const;

    /// Runs `query` through a collector manager (one collector per leaf slice)
    /// and reduces the results.
    [[nodiscard]] Result<TopDocs> search_with_collector_manager(const Query &query,
                                                                const CollectorManager &manager) const;

    /// Runs `query` through a sorted collector manager (one collector per leaf
    /// slice) and merges the slices by their sort values.
    [[nodiscard]] Result<TopFieldDocs> search_with_field_collector_manager(const Query &query,
                                                                           const TopFieldCollectorManager &manager) const;

    /// Feeds every leaf into `collector`; the collector keeps the results.
    [[nodiscard]] VoidResult search_with_collector(const Query &query, Collector &collector) const;

    /// Rewrites `query` until it reaches its final form.
    ///
    /// Rewrites `query` until it reaches its final form.
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const Query &query) const;

    /// Rewrites `query`, first wrapping it in a
    /// [`ConstantScoreQuery`](:constant_score_query) when the caller does not need
    /// scores, so that its extra rewrite rules apply (a scoreless `PrefixQuery`
    /// then reaches the doc-values rewrite, for instance).
    ///
    /// Port of `IndexSearcher::rewrite_with_needs_scores`.
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite_with_needs_scores(const Query &query,
                                                                          bool needs_scores) const;

private:
    /// Collects the leaf at `ord`, restricted to the document range
    /// `[min_doc_id, max_doc_id)`.
    [[nodiscard]] VoidResult search_leaf(int32_t ord, int32_t min_doc_id, int32_t max_doc_id, Weight &weight,
                                         Collector &collector) const;

    /// Runs one task per leaf slice, each with a collector of `manager`, and
    /// answers those collectors in slice order (unreduced).
    [[nodiscard]] Result<std::vector<std::unique_ptr<Collector>>>
    search_slices(const Query &query, const CollectorManager &manager) const;

    /// The shared body of [`search`] and [`search_after`]: sorts the hits and
    /// skips the ones before `after` when there is a cursor.
    [[nodiscard]] Result<TopFieldDocs> search_sorted(const FieldDoc *after, const Query &query, const Sort &sort,
                                                     int32_t num_hits) const;

    std::shared_ptr<IndexReader> reader_;
    std::shared_ptr<Similarity> similarity_;
    std::shared_ptr<TaskExecutor> task_executor_;
    std::shared_ptr<QueryCache> query_cache_;
    std::shared_ptr<const QueryCachingPolicy> caching_policy_;
};

} // namespace lucene
