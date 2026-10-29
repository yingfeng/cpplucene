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

export module lucene_core:near_spans_unordered;

import :conjunction_spans;
import :spans;
import :types;

import std;

namespace lucene {

/// Checks that the sub spans of one document are close together in any order,
/// with the positions between them (the span lengths subtracted) summing to at
/// most `allowed_slop`.
///
/// Port of `NearSpansUnordered` (`queries/span/near_spans_unordered.rs`).  The
/// reported span starts at the smallest sub span start and ends at the largest
/// sub span end.
///
/// **Deviation:** the reference implementation keeps the sub spans in a
/// `PriorityQueue` ordered by `(start, end)` and re-heapifies after each step;
/// this port scans the (small) sub span list for the smallest one.  The window
/// arithmetic — `totalSpanLength` and `maxEndPosition` — is the same, so the
/// acceptance test and the reported positions match.
export class NearSpansUnordered final : public ConjunctionSpans {
public:
    NearSpansUnordered(int32_t allowed_slop, std::vector<std::unique_ptr<Spans>> sub_spans);

    [[nodiscard]] Result<int32_t> next_start_position() override;
    [[nodiscard]] int32_t start_position() const override;
    [[nodiscard]] int32_t end_position() const override;
    [[nodiscard]] int32_t width() const override;

    [[nodiscard]] VoidResult collect(SpanCollector &collector) override;

    /// `spans1` starts before `spans2`, or at the same position and ends before
    /// it.
    [[nodiscard]] static bool positions_ordered(const Spans &spans1, const Spans &spans2);

protected:
    [[nodiscard]] Result<bool> two_phase_current_doc_matches() override;

private:
    /// The sub span with the smallest `(start, end)`.
    [[nodiscard]] Spans &top() const;

    /// Positions every sub span on its first position of the current document and
    /// accumulates the window totals.
    [[nodiscard]] VoidResult start_document();

    /// Advances the sub span at the top of the window; `false` when it has no
    /// further position.
    [[nodiscard]] Result<bool> next_position();

    [[nodiscard]] bool at_match() const;

    int32_t allowed_slop_;
    int32_t total_span_length_{0};
    int32_t max_end_position_{-1};
};

} // namespace lucene
