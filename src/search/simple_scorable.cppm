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

export module lucene_core:simple_scorable;

import :types;
import std;

import :error;
import :scorable;

namespace lucene {

/// A [`Scorable`] whose score is simply stored, with no scorer behind it.
///
/// Port of `SimpleScorable` (`core/search/simple_scorable.rs`).
///
/// It keeps the minimum competitive score a caller sets, which is what the
/// constant score collectors ask for.
export class SimpleScorable final : public Scorable {
public:
    /// The score of the current document.
    [[nodiscard]] Result<float> score() override { return score_; }

    /// Records the minimum competitive score.
    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override {
        min_competitive_score_ = min_score;
        return {};
    }

    /// Overwrites the stored score.
    [[nodiscard]] VoidResult set_score(float score) override {
        score_ = score;
        return {};
    }

    /// The stored score.
    [[nodiscard]] float value() const noexcept { return score_; }

    /// The last minimum competitive score that was set.
    [[nodiscard]] float min_competitive_score() const noexcept { return min_competitive_score_; }

private:
    float score_{0.0F};
    float min_competitive_score_{0.0F};
};

} // namespace lucene
