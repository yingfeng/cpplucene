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

export module lucene_core:top_field_collector_manager;

import :types;

import std;

import :collector;
import :collector_manager;
import :error;
import :max_score_accumulator;
import :sort_field;
import :top_docs;
import :top_field_docs;

namespace lucene {

/// Creates [`TopFieldCollector`](lucene_core:top_field_collector)s, one per leaf
/// slice, and merges their sorted results.
///
/// Port of `TopFieldCollectorManager`
/// (`core/search/top_field_collector_manager.rs`).
///
/// The collectors share a [`MaxScoreAccumulator`](lucene_core:max_score_accumulator),
/// which is how a relevance sort skips the hits that can no longer win.  The
/// reference implementation's generic `after` parameter collapses into
/// `std::optional<`[`FieldDoc`](:top_field_docs)`>`: [`create`] builds first-page
/// collectors, [`with_after`] builds paging ones.
///
/// A manager carries the cross-slice state, so a new one is needed for each
/// search.
export class TopFieldCollectorManager final : public CollectorManager {
public:
    /// Creates a first-page manager for `num_hits` hits sorted by `sort`.
    ///
    /// `total_hits_threshold` is how many hits are counted exactly, like
    /// [`TopFieldCollector::create`](lucene_core:top_field_collector); it defaults
    /// to [`IndexSearcher::TOTAL_HITS_THRESHOLD`](lucene_core:index_searcher).
    [[nodiscard]] static Result<std::unique_ptr<TopFieldCollectorManager>>
    create(const Sort &sort, int32_t num_hits, size_t total_hits_threshold = 1000);

    /// Creates a manager that keeps only the hits after `after`.
    [[nodiscard]] static Result<std::unique_ptr<TopFieldCollectorManager>>
    with_after(const Sort &sort, int32_t num_hits, const FieldDoc &after, size_t total_hits_threshold = 1000);

    [[nodiscard]] Result<std::unique_ptr<Collector>> new_collector() const override;

    /// The plain hits of [`reduce_field_docs`], for callers that do not need the
    /// sort values.
    [[nodiscard]] Result<std::vector<TopDocs>>
    reduce(std::vector<std::unique_ptr<Collector>> collectors) const override;

    /// The scored and sorted hits: merges the per-slice results by their sort
    /// values.
    [[nodiscard]] Result<TopFieldDocs> reduce_field_docs(std::vector<std::unique_ptr<Collector>> collectors) const;

    [[nodiscard]] const Sort &sort() const noexcept { return sort_; }

    [[nodiscard]] int32_t num_hits() const noexcept { return num_hits_; }

private:
    TopFieldCollectorManager(Sort sort, int32_t num_hits, std::optional<FieldDoc> after, size_t total_hits_threshold);

    Sort sort_;
    int32_t num_hits_{0};
    std::optional<FieldDoc> after_;
    size_t total_hits_threshold_{0};
    /// The maximum score the collectors of this search share.
    std::shared_ptr<MaxScoreAccumulator> min_score_acc_;
};

} // namespace lucene
