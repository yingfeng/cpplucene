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

export module lucene_core:conjunction_spans;

import :conjunction_disi;
import :doc_id_set_iterator;
import :spans;
import :types;

import std;

namespace lucene {

/// The base of the span queries that need several sub spans to match the same
/// document.
///
/// Port of `ConjunctionSpans` (`queries/span/conjunction_spans.rs`).  The
/// documents are driven by the conjunction of the sub spans, and each candidate
/// is then confirmed on positions by [`two_phase_current_doc_matches`]; the
/// documents that survive both steps are what [`next_doc`] returns.
///
/// **Deviation:** no two-phase view is published (the reference implementation
/// offers one so the outer scorer can prune candidates).  The positions are
/// already checked in [`next_doc`] / [`advance`], so only pruning is lost, and
/// [`positions_cost`] answers `1` instead of throwing.
export class ConjunctionSpans : public Spans {
public:
    /// `sub_spans` must hold at least two spans, all positioned on the current
    /// document.
    explicit ConjunctionSpans(std::vector<std::unique_ptr<Spans>> sub_spans);

    [[nodiscard]] int32_t doc_id() const override { return conjunction_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return conjunction_->cost(); }
    [[nodiscard]] float positions_cost() const override { return 1.0F; }

    [[nodiscard]] const std::vector<std::unique_ptr<Spans>> &sub_spans() const noexcept { return sub_spans_; }

protected:
    /// Whether the sub spans agree on a position in the current document.
    [[nodiscard]] virtual Result<bool> two_phase_current_doc_matches() = 0;

    std::vector<std::unique_ptr<Spans>> sub_spans_;
    std::unique_ptr<ConjunctionDISI> conjunction_;
    bool at_first_in_current_doc_{true};
    bool one_exhausted_in_current_doc_{false};

private:
    [[nodiscard]] Result<int32_t> to_match_doc();
};

} // namespace lucene
