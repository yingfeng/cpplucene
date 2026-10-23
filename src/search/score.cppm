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

export module lucene_core:score;

import :types;
import std;

import :error;
import :scorable;

namespace lucene {

/// A [`Scorable`] of a fixed score, for the bulk scorers that have to hand a
/// score to `LeafCollector::collect` without owning a scorer.
///
/// Port of `Score` (`core/search/score.rs`).
export class Score final : public Scorable {
public:
    explicit Score(float score) : score_(score) {}

    /// The fixed score.
    [[nodiscard]] Result<float> score() override { return score_; }

    /// Overwrites the fixed score.
    [[nodiscard]] VoidResult set_score(float score) override {
        score_ = score;
        return {};
    }

    /// The current fixed score.
    [[nodiscard]] float value() const noexcept { return score_; }

private:
    float score_{0.0F};
};

} // namespace lucene
