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

export module lucene_core:total_hit_count_collector;

import :types;
import std;

import :collector;
import :error;
import :leaf_collector;
import :leaf_reader_context;
import :score_mode;

namespace lucene {

export class Weight;
export class IndexSearcher;

/// Counts the hits of a query without keeping them, the collector behind a
/// `count` search.
///
/// Port of `TotalHitCountCollector` (`core/search/total_hit_count_collector.rs`).
///
/// When the [`Weight`] can count a leaf in sub-linear time
/// ([`Weight::count`](:weight) answers a non-negative number), the leaf is not
/// visited at all: the collector is terminated for that leaf instead.
///
/// **Deviations:** the reference implementation's partition-aware early
/// termination map and `collect_stream` are not ported — the port has no
/// `DocIdStream` — and there is no `TotalHitCountCollectorManager`, because the
/// port's [`CollectorManager`](:collector_manager) is typed to produce `TopDocs`.
export class TotalHitCountCollector final : public Collector {
public:
    /// A collector that counts every hit.
    TotalHitCountCollector() = default;

    [[nodiscard]] Result<std::unique_ptr<LeafCollector>>
    get_leaf_collector(const LeafReaderContext &context, const Weight *weight,
                       const IndexSearcher &searcher) override;

    /// No scores are needed to count.
    [[nodiscard]] ScoreMode score_mode() const override { return ScoreMode::CompleteNoScores; }

    /// How many hits matched.
    [[nodiscard]] int32_t total_hits() const noexcept { return total_hits_; }

    /// Adds the hits of a leaf that was counted without visiting it.
    void add_hits(int32_t hits) noexcept { total_hits_ += hits; }

private:
    int32_t total_hits_{0};
};

} // namespace lucene
