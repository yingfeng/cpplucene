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

export module lucene_core:exact_phrase_matcher;

import :types;
import std;

import :error;
import :phrase_matcher;

namespace lucene {

/// Matches a phrase whose terms have to be adjacent, i.e. a query with no slop.
///
/// Port of `ExactPhraseMatcher` (`core/search/exact_phrase_matcher.rs`).
///
/// The first posting leads: its positions are tried in order, and the other
/// postings are advanced to the position the lead implies.
///
/// **Deviation:** the reference implementation sorts the postings by frequency
/// and wraps them in impacts enums; here the caller supplies the postings in
/// ascending frequency order and the matcher advances them directly.
export class ExactPhraseMatcher final : public PhraseMatcherBase {
public:
    ExactPhraseMatcher(std::vector<PostingsAndFreq> postings, float match_cost);

    // ---- PhraseMatcher ----

    [[nodiscard]] Result<float> max_freq() override;
    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> next_match() override;
    [[nodiscard]] float sloppy_weight() const override { return 1.0F; }
    [[nodiscard]] int32_t start_position() const override;
    [[nodiscard]] int32_t end_position() const override;
    [[nodiscard]] Result<int32_t> start_offset() const override;
    [[nodiscard]] Result<int32_t> end_offset() const override;
    [[nodiscard]] float get_match_cost() const override { return match_cost_; }

private:
    /// The position state of one posting.
    struct Slot {
        /// Position inside the phrase.
        int32_t offset{0};
        /// Current position in the document.
        int32_t pos{-1};
        /// Number of positions of the current document.
        int32_t freq{0};
        /// How many positions have been read.
        int32_t upto{0};
    };

    /// Advances the position cursor of `index` to the first position at or after
    /// `target`; `false` when the document has no such position.
    [[nodiscard]] Result<bool> advance_position(size_t index, int32_t target);

    std::vector<Slot> slots_;
    float match_cost_{0.0F};
};

} // namespace lucene
