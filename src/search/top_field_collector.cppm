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
import :max_score_accumulator;
import :leaf_metadata;
import :leaf_reader;
import :leaf_reader_context;
import :score_mode;
import :sort_field;
import :top_field_docs;
import :total_hits;

namespace lucene {

export class Weight;

/// Collects the `num_hits` best hits of a search ordered by a [`Sort`].
///
/// Port of `TopFieldCollector`/`SimpleFieldCollector`/`PagingFieldCollector`
/// (`core/search/top_field_collector.rs`).
///
/// **Deviations:** the index-sort early termination is decided per leaf instead
/// of once from the first leaf. `after` paging keeps the reference behaviour of counting every
/// match in [`total_hits`], including the hits of the previous pages that the
/// cursor makes it skip.
export class TopFieldCollector final : public Collector {
public:
    /// The leaf collector; a nested class so it can reach the queue.
    class Leaf;

    /// A collector of `num_hits` hits sorted by `sort`; when `after` is not null,
    /// the hits that sort before it are skipped, which is what deep paging
    /// (`search_after`) needs.  `after` is borrowed for the call only.
    ///
    /// `total_hits_threshold` is how many hits are counted exactly: once a search
    /// finds more, the reported total becomes a lower bound (relation
    /// `GreaterThanOrEqualTo`) and, when the sort is a prefix of the index sort,
    /// collection stops early.  The default counts every hit, which is what a
    /// direct caller of this method used to get.
    /// `min_score_acc` is the accumulator the collectors of one search share: a
    /// collector publishes the score no hit can beat any more
    /// ([`MaxScoreAccumulator`](lucene_core:max_score_accumulator)) and reads the
    /// best one the other collectors published.  A null accumulator means the
    /// search runs on one collector, which needs no sharing.
    [[nodiscard]] static Result<std::unique_ptr<TopFieldCollector>>
    create(const Sort &sort, int32_t num_hits, const FieldDoc *after = nullptr,
           size_t total_hits_threshold = std::numeric_limits<size_t>::max(),
           std::shared_ptr<MaxScoreAccumulator> min_score_acc = nullptr);

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
    /// `true` when the collector has an `after` cursor.
    bool paging_{false};
    /// The document of the `after` cursor, in the global numbering.
    int32_t after_doc_{0};
    /// How many hits are counted exactly; never below `num_hits`.
    size_t total_hits_threshold_{std::numeric_limits<size_t>::max()};
    /// Set when the sort starts with the score, in the natural (descending)
    /// direction and with a finite threshold: only then may a hit be skipped on its
    /// score.
    bool can_set_min_score_{false};
    /// The score no hit can beat any more, as published to the scorer.
    float min_competitive_score_{0.0F};
    /// The accumulator shared with the other collectors of the same search.
    std::shared_ptr<MaxScoreAccumulator> min_score_acc_;
    /// Whether [`total_hits_`] is exact (`EqualTo`) or a lower bound
    /// (`GreaterThanOrEqualTo`).
    TotalHits::Relation total_hits_relation_{TotalHits::Relation::EqualTo};
};

/// Whether collection may stop at the first non-competitive document, which is
/// what a sort that is a prefix of the index sort allows: documents are then
/// visited in sort order, so every later document is non-competitive as well.
///
/// Port of `can_early_terminate` (`core/search/top_field_collector.rs`).
[[nodiscard]] bool can_early_terminate(const Sort &search_sort, const Sort *index_sort);

} // namespace lucene
