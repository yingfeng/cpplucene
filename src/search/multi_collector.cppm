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

export module lucene_core:multi_collector;

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

/// Forwards every hit to several collectors, so that one search can feed more
/// than one of them.
///
/// Port of `MultiCollector` (`core/search/multi_collector.rs`).
///
/// A hit is handed to every wrapped collector.  When all of them may skip
/// low-scoring hits, the smallest threshold any of them asks for is passed on to
/// the scorer; otherwise the thresholds are swallowed, so one collector's limit
/// cannot hide documents another one still needs.
///
/// **Deviations:** `DocIdStream` is not fanned out either — a stream is terminal,
/// so the per-document `collect` is used instead (the reference implementation
/// says the same and does not propagate it).  Reporting no competitive iterator is
/// *not* a deviation: the reference implementation's multi leaf collector answers
/// `EmptyDISI` as well, because it cannot merge the iterators of its children.
export class MultiCollector final : public Collector {
public:
    /// A collector over `collectors`, which must not be empty and have to outlive
    /// it.
    [[nodiscard]] static Result<std::unique_ptr<MultiCollector>> wrap(std::vector<Collector *> collectors);

    [[nodiscard]] Result<std::unique_ptr<LeafCollector>>
    get_leaf_collector(const LeafReaderContext &context, const Weight *weight,
                       const IndexSearcher &searcher) override;

    /// The most demanding mode of the wrapped collectors.
    [[nodiscard]] ScoreMode score_mode() const override { return score_mode_; }

    [[nodiscard]] VoidResult set_weight(const Weight *weight) override;

    /// The wrapped collectors.
    [[nodiscard]] const std::vector<Collector *> &collectors() const noexcept { return collectors_; }

private:
    explicit MultiCollector(std::vector<Collector *> collectors);

    std::vector<Collector *> collectors_;
    ScoreMode score_mode_{ScoreMode::CompleteNoScores};
    /// Whether every wrapped collector may skip low-scoring hits.
    bool skip_non_competitive_scores_{false};
};

} // namespace lucene
