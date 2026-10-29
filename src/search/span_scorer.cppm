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

export module lucene_core:span_scorer;

import :numeric_doc_values;
import :scorer;
import :similarity;
import :spans;
import :types;

import std;

namespace lucene {

/// Scores the documents a [`Spans`] produces, with the so-called sloppy
/// frequency: every match at position pair `(start, end)` adds `1 / (1 + width)`,
/// so narrower matches score higher.
///
/// Port of `SpanScorer` (`queries/span/span_scorer.rs`).
///
/// **Deviation:** the reference implementation offers the spans' two-phase view
/// to the outer scorer; this port does not, because `ConjunctionSpans` already
/// aligns the positions inside `next_doc` / `advance`, so the two-phase step
/// would only buy pruning, not correctness.  Multi-term span queries that do need
/// the pruning can add it later without changing this class.
export class SpanScorer final : public Scorer {
public:
    SpanScorer(std::unique_ptr<Spans> spans, std::shared_ptr<SimScorer> sim_scorer,
               std::unique_ptr<NumericDocValues> norms);

    [[nodiscard]] Spans &spans() const noexcept { return *spans_; }

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override { return spans_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return spans_->next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return spans_->advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return spans_->cost(); }

    // ---- Scorer ----

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return spans_->doc_id(); }

    /// Always positive infinity: a span scorer cannot bound its score without
    /// walking the positions.
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override;

    /// [`TwoPhaseState::No`]: see the class notes.
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }

    // ---- Scorable ----

    [[nodiscard]] Result<float> score() override;
    [[nodiscard]] Result<float> smoothing_score(int32_t /*doc_id*/) override { return score(); }

    /// The sloppy frequency of the current document, computed on demand.
    [[nodiscard]] Result<float> sloppy_freq();

private:
    [[nodiscard]] Result<float> score_current_doc();
    [[nodiscard]] VoidResult set_freq_current_doc();
    [[nodiscard]] VoidResult ensure_freq();

    std::unique_ptr<Spans> spans_;
    std::shared_ptr<SimScorer> sim_scorer_;
    std::unique_ptr<NumericDocValues> norms_;
    float freq_{0.0F};
    int32_t last_scored_doc_{-1};
};

} // namespace lucene
