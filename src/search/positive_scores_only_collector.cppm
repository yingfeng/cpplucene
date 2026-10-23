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

export module lucene_core:positive_scores_only_collector;

import :types;
import std;

import :collector;
import :error;
import :filter_leaf_collector;
import :leaf_collector;
import :leaf_reader_context;
import :score_mode;

namespace lucene {

export class Weight;
export class IndexSearcher;

/// A collector wrapper that only forwards hits whose score is strictly positive.
///
/// Port of `PositiveScoresOnlyCollector`
/// (`core/search/positive_scores_only_collector.rs`).
///
/// **Deviation:** the reference implementation wraps its leaf collector in a
/// `ScoreCachingWrappingLeafCollector` so that the score is only computed once;
/// the port forwards the scorer as it is, because a scorer costs nothing to ask
/// twice for the same document.
export class PositiveScoresOnlyCollector final : public Collector {
public:
    /// Wraps `inner`, which must outlive this collector.
    explicit PositiveScoresOnlyCollector(Collector &inner) : inner_(&inner) {}

    [[nodiscard]] Result<std::unique_ptr<LeafCollector>>
    get_leaf_collector(const LeafReaderContext &context, const Weight *weight,
                       const IndexSearcher &searcher) override;

    /// Needs the scores unless the wrapped collector visits every match.
    [[nodiscard]] ScoreMode score_mode() const override {
        return score_mode_is_exhaustive(inner_->score_mode()) ? ScoreMode::Complete : ScoreMode::TopScores;
    }

    [[nodiscard]] VoidResult set_weight(const Weight *weight) override { return inner_->set_weight(weight); }

private:
    Collector *inner_{nullptr};
};

} // namespace lucene
