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

export module lucene_core:term_spans;

import :postings_enum;
import :spans;
import :term;
import :types;

import std;

namespace lucene {

/// The spans of a single term: one position per occurrence in the document.
///
/// Port of `TermSpans` (`queries/span/term_spans.rs`).  It does **not** offer a
/// two-phase view, because a document that the postings reached has `freq >= 1`
/// and therefore at least one position.
export class TermSpans final : public Spans {
public:
    TermSpans(std::unique_ptr<PostingsEnum> postings, Term term, float positions_cost);

    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int64_t> cost() const override;

    [[nodiscard]] Result<int32_t> next_start_position() override;
    [[nodiscard]] int32_t start_position() const override { return position_; }
    [[nodiscard]] int32_t end_position() const override;
    [[nodiscard]] int32_t width() const override { return 0; }

    [[nodiscard]] VoidResult collect(SpanCollector &collector) override;
    [[nodiscard]] float positions_cost() const override { return positions_cost_; }

    [[nodiscard]] const Term &term() const noexcept { return term_; }
    [[nodiscard]] PostingsEnum &postings() const noexcept { return *postings_; }

private:
    std::unique_ptr<PostingsEnum> postings_;
    Term term_;
    int32_t doc_{-1};
    int32_t freq_{0};
    int32_t count_{0};
    int32_t position_{-1};
    float positions_cost_;
};

} // namespace lucene
