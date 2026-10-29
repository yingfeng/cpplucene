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

export module lucene_core:near_spans_ordered;

import :conjunction_spans;
import :spans;
import :types;

import std;

namespace lucene {

/// Checks that the sub spans appear in order, with the gaps between them summing
/// to at most `allowed_slop`.
///
/// Port of `NearSpansOrdered` (`queries/span/near_spans_ordered.rs`).  The span
/// it reports starts at the first sub span's start and ends at the last one's
/// end; its width is the sum of the gaps.
export class NearSpansOrdered final : public ConjunctionSpans {
public:
    NearSpansOrdered(int32_t allowed_slop, std::vector<std::unique_ptr<Spans>> sub_spans);

    [[nodiscard]] Result<int32_t> next_start_position() override;
    [[nodiscard]] int32_t start_position() const override {
        return at_first_in_current_doc_ ? -1 : match_start_;
    }
    [[nodiscard]] int32_t end_position() const override { return at_first_in_current_doc_ ? -1 : match_end_; }
    [[nodiscard]] int32_t width() const override { return match_width_; }

    [[nodiscard]] VoidResult collect(SpanCollector &collector) override;

protected:
    [[nodiscard]] Result<bool> two_phase_current_doc_matches() override;

private:
    /// Orders the sub spans of the current document, advancing the ones after the
    /// first as little as possible.  `false` means one of them is exhausted.
    [[nodiscard]] Result<bool> stretch_to_order();

    int32_t match_start_{-1};
    int32_t match_end_{-1};
    int32_t match_width_{-1};
    int32_t allowed_slop_;
};

} // namespace lucene
