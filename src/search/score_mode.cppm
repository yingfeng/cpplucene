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

export module lucene_core:score_mode;

import :types;

namespace lucene {

/// What a collector needs from a scorer.
///
/// Port of `ScoreMode` (`core/search/score_mode.rs`).
export enum class ScoreMode : int32_t {
    /// All matches are visited and their scores are collected.
    Complete = 0,
    /// All matches are visited, but no scores are needed.
    CompleteNoScores = 1,
    /// Matching may stop early on non-competitive hits.
    TopScores = 2,
    /// Only the top documents are visited; scores are not needed.
    TopDocs = 3,
    /// Only the top documents are visited, with a tie-break on the score.
    TopDocsWithScores = 4,
};

/// `true` when the mode needs the score of every collected document.
[[nodiscard]] bool score_mode_needs_scores(ScoreMode mode) noexcept;

/// `true` when every document must be visited.
[[nodiscard]] bool score_mode_is_exhaustive(ScoreMode mode) noexcept;

} // namespace lucene
