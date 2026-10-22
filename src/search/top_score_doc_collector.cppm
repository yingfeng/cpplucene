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

export module lucene_core:top_score_doc_collector;

import :types;

import std;

import :collector;
import :error;
import :hit_queue;
import :leaf_collector;
import :leaf_reader_context;
import :max_score_accumulator;
import :score_doc;
import :score_mode;
import :top_docs;
import :top_docs_collector;
import :total_hits;

namespace lucene {

export class IndexSearcher;
export class Weight;

/// Collects the top-scoring hits and returns them as [`TopDocs`], sorted by
/// score descending and then by document number ascending.
///
/// Port of `TopScoreDocCollector` (`core/search/top_score_doc_collector.rs`).
///
/// The hit queue is pre-filled with sentinels, so the collector always replaces
/// the top instead of inserting, and it can skip "not competitive" documents as
/// soon as the queue is full.
///
/// **Deviations** (C++ mechanics only):
///  * rlucene has a `const CHECK_AFTER: bool` template parameter to pick the
///    first-page (no cursor) code path at compile time; the port keeps it as a
///    construction-time flag, because the port has no compile-time
///    specialisation to gain from;
///  * the state the leaf collector needs is exposed through accessors instead of
///    a same-module borrow.
///
/// **NOTE:** `NaN` and `-infinity` are not valid scores; hits with such scores
/// are not collected properly, exactly like Lucene.
export class TopScoreDocCollector final : public Collector {
public:
    /// Creates a collector.
    ///
    /// `after` is the cursor of the previous page (only honoured when
    /// `check_after`), `total_hits_threshold` is how many hits to count exactly
    /// before the count becomes a lower bound, and `min_score_acc` shares the
    /// minimum competitive score across leaves (null disables it).
    [[nodiscard]] static Result<std::unique_ptr<TopScoreDocCollector>> create(
            size_t num_hits, std::optional<ScoreDoc> after, size_t total_hits_threshold,
            std::shared_ptr<MaxScoreAccumulator> min_score_acc, bool check_after = true);

    [[nodiscard]] Result<std::unique_ptr<LeafCollector>>
    get_leaf_collector(const LeafReaderContext &context, const Weight *weight, const IndexSearcher &searcher) override;

    [[nodiscard]] ScoreMode score_mode() const override;

    /// The collected top docs, consuming the queue.
    [[nodiscard]] Result<TopDocs> top_docs();

    /// The collected top docs, skipping the first `start` hits.
    [[nodiscard]] Result<TopDocs> top_docs_with_start(int32_t start);

    /// The collected top docs in `[start, start + how_many)`.
    [[nodiscard]] Result<TopDocs> top_docs_with_start_limit(int32_t start, int32_t how_many);

    /// Number of real hits in the queue (the sentinels do not count).
    [[nodiscard]] size_t top_docs_size() const;

    // ---- state shared with the leaf collectors ----

    [[nodiscard]] TopDocsCollectorBase &base() noexcept { return base_; }
    [[nodiscard]] const TopDocsCollectorBase &base() const noexcept { return base_; }
    [[nodiscard]] const std::optional<ScoreDoc> &after() const noexcept { return after_; }
    [[nodiscard]] bool check_after() const noexcept { return check_after_; }
    [[nodiscard]] size_t total_hits_threshold() const noexcept { return total_hits_threshold_; }
    [[nodiscard]] const std::shared_ptr<MaxScoreAccumulator> &min_score_acc() const noexcept {
        return min_score_acc_;
    }

private:
    TopScoreDocCollector(TopDocsCollectorBase base, std::optional<ScoreDoc> after, size_t total_hits_threshold,
                         std::shared_ptr<MaxScoreAccumulator> min_score_acc, bool check_after);

    TopDocsCollectorBase base_;
    std::optional<ScoreDoc> after_;
    size_t total_hits_threshold_;
    std::shared_ptr<MaxScoreAccumulator> min_score_acc_;
    bool check_after_;
};

} // namespace lucene
