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

export module lucene_core:top_docs_collector;

import :types;

import std;

import :error;
import :hit_queue;
import :score_doc;
import :top_docs;
import :total_hits;

namespace lucene {

/// The state and the `TopDocs` building half of every collector that returns a
/// [`TopDocs`].
///
/// Port of `TopDocsCollectorBase` + the default `TopDocsCollector` methods
/// (`core/search/top_docs_collector.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * `TopDocsCollector` is a trait with associated `Item`/`Cmp`/`TopDocsLike`
///    types; the port has one `TopDocs` over [`ScoreDoc`] and a [`HitQueue`], so
///    the base is a plain class (the field-sorting collector, which is what
///    needs the genericity, is not ported yet);
///  * `pq` / `total_hits` / `total_hits_relation` are public, like the reference implementation's
///    `pub(crate)` fields: the leaf collectors live in their own translation unit
///    and update them.
export class TopDocsCollectorBase {
public:
    explicit TopDocsCollectorBase(HitQueue queue);

    virtual ~TopDocsCollectorBase() = default;

    /// The "no hits" result.
    [[nodiscard]] static TopDocs empty_top_docs();

    /// Number of hits the queue can still return: `min(total_hits, queue size)`.
    [[nodiscard]] virtual size_t top_docs_size() const;

    /// The collected top docs, consuming the queue.
    [[nodiscard]] virtual Result<TopDocs> top_docs();

    /// The collected top docs, skipping the first `start` hits.
    [[nodiscard]] virtual Result<TopDocs> top_docs_with_start(int32_t start);

    /// The collected top docs in `[start, start + how_many)`, consuming the
    /// queue.  Only valid once per search.
    [[nodiscard]] virtual Result<TopDocs> top_docs_with_start_limit(int32_t start, int32_t how_many);

    /// Builds the result from `results` (`std::nullopt` = "nothing to return").
    ///
    /// **Deviation:** the reference implementation's `TopScoreDocCollector` overrides this to keep
    /// the total hit count even when there are no hits to return, while the trait
    /// default answers "no hits at all".  The port has no other `TopDocs`
    /// collector yet, so the base carries the `TopScoreDocCollector` semantics.
    [[nodiscard]] virtual TopDocs new_top_docs(std::optional<std::vector<ScoreDoc>> results, int32_t start) const;

    /// The number of documents that matched the query.
    size_t total_hits{0};
    /// Whether [`total_hits`] is exact or a lower bound.
    TotalHits::Relation total_hits_relation{TotalHits::Relation::EqualTo};
    /// The hits, worst first.
    HitQueue pq;

protected:
    /// Moves the `how_many` most competitive hits into `results`, in order.
    [[nodiscard]] VoidResult populate_results(std::span<ScoreDoc> results, size_t how_many);
};

} // namespace lucene
