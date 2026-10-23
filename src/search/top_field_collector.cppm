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

export module lucene_core:top_field_collector;

import :types;
import std;

import :collector;
import :error;
import :field_comparator;
import :field_value_hit_queue;
import :leaf_collector;
import :leaf_reader_context;
import :score_mode;
import :sort_field;
import :top_field_docs;

namespace lucene {

export class Weight;

/// Collects the `num_hits` best hits of a search ordered by a [`Sort`].
///
/// Port of `TopFieldCollector`/`SimpleFieldCollector`
/// (`core/search/top_field_collector.rs`).
///
/// **Deviations:** the paging (`search_after`) and total-hits-threshold paths are
/// not ported, and neither is the early termination of an index sort, so every
/// match is visited.
export class TopFieldCollector final : public Collector {
public:
    /// The leaf collector; a nested class so it can reach the queue.
    class Leaf;

    /// A collector of `num_hits` hits sorted by `sort`.
    [[nodiscard]] static Result<std::unique_ptr<TopFieldCollector>> create(const Sort &sort, int32_t num_hits);

    // ---- Collector ----

    [[nodiscard]] Result<std::unique_ptr<LeafCollector>>
    get_leaf_collector(const LeafReaderContext &context, const Weight *weight,
                       const IndexSearcher &searcher) override;

    /// Scores are needed when the sort reads them; otherwise every match is
    /// visited without scores.
    [[nodiscard]] ScoreMode score_mode() const override {
        return needs_scores_ ? ScoreMode::TopScores : ScoreMode::CompleteNoScores;
    }

    /// The hits, best first, skipping the first `start` of them.
    [[nodiscard]] Result<TopFieldDocs> top_docs(int32_t start = 0);

    /// How many documents matched.
    [[nodiscard]] int32_t total_hits() const noexcept { return total_hits_; }

    /// The queue, for the leaf collector.
    [[nodiscard]] FieldValueHitQueue &queue() noexcept { return *queue_; }

private:
    TopFieldCollector(Sort sort, std::unique_ptr<FieldValueHitQueue> queue);

    Sort sort_;
    std::unique_ptr<FieldValueHitQueue> queue_;
    int32_t total_hits_{0};
    bool needs_scores_{false};
};

} // namespace lucene
