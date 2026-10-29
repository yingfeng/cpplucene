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

export module lucene_core:large_num_hits_top_docs_collector;

import :collector;
import :hit_queue;
import :score_doc;
import :score_mode;
import :top_docs;
import :types;

import std;

namespace lucene {

/// The per-leaf collector of [`LargeNumHitsTopDocsCollector`].
export class LargeNumHitsLeafCollector;

/// A top-N collector that stays cheap when N is large.
///
/// Port of `LargeNumHitsTopDocsCollector` (`sandbox/search/`).  It keeps a plain
/// vector of hits until `requested_hit_count` hits were seen, and only then
/// switches to a priority queue (and starts comparing against its minimum
/// competitive score).  A normal `TopScoreDocCollector` would pay the heap cost
/// for every hit from the start.
export class LargeNumHitsTopDocsCollector final : public Collector {
public:
    explicit LargeNumHitsTopDocsCollector(size_t requested_hit_count);

    /// The best `min(total_hits, requested_hit_count)` hits.
    [[nodiscard]] Result<TopDocs> top_docs();

    /// The best `how_many` hits; `how_many` must be in `[1, total_hits]`.
    [[nodiscard]] Result<TopDocs> top_docs_with_how_many(size_t how_many);

    /// How many documents matched.
    [[nodiscard]] size_t total_hits() const noexcept { return total_hits_; }

    /// How many hits were requested at construction.
    [[nodiscard]] size_t requested_hit_count() const noexcept { return requested_hit_count_; }

    // ---- Collector ----

    [[nodiscard]] Result<std::unique_ptr<LeafCollector>> get_leaf_collector(const LeafReaderContext &context,
                                                                           const Weight *weight,
                                                                           const IndexSearcher &searcher) override;

    /// Always [`ScoreMode::Complete`]: the collector never trades scores for speed.
    [[nodiscard]] ScoreMode score_mode() const override { return ScoreMode::Complete; }

private:
    friend class LargeNumHitsLeafCollector;

    /// Moves the `how_many` most competitive hits into the result, in order.
    [[nodiscard]] Result<std::vector<ScoreDoc>> populate_results(size_t how_many);

    size_t requested_hit_count_;
    /// Hits collected before the queue was built; `nullopt` once converted.
    std::optional<std::vector<ScoreDoc>> hits_{};
    /// The queue, built lazily when the vector fills up.
    std::optional<HitQueue> pq_{};
    size_t total_hits_{0};
};

} // namespace lucene
