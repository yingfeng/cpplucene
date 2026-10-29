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

export module lucene_core:filter_spans;

import :spans;
import :types;

import std;

namespace lucene {

/// What [`FilterSpans::accept`] decides about a candidate span.
///
/// Named `SpanAcceptStatus` rather than the reference implementation's nested
/// `FilterSpans.AcceptStatus`, because `FilteredTermsEnum` already exports an
/// `AcceptStatus` in this namespace.
export enum class SpanAcceptStatus : uint8_t {
    /// Keep the span.
    kYes,
    /// Drop the span and try the next one in this document.
    kNo,
    /// Drop the span and move on to the next document.
    kNoMoreInCurrentDoc,
};

/// A [`Spans`] that keeps only the spans its subclass accepts.
///
/// Port of `FilterSpans` (`queries/span/filter_spans.rs`).  The rejection happens
/// while the documents are produced, so a document without an accepted span is
/// skipped entirely.
///
/// **Deviation:** the two-phase view is not published (the reference
/// implementation forwards the wrapped spans' approximation); this port checks
/// the spans eagerly and answers `positions_cost` with the wrapped spans' cost.
export class FilterSpans : public Spans {
public:
    explicit FilterSpans(std::unique_ptr<Spans> input);

    [[nodiscard]] int32_t doc_id() const override { return input_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return input_->cost(); }

    [[nodiscard]] Result<int32_t> next_start_position() override;
    [[nodiscard]] int32_t start_position() const override {
        return at_first_in_current_doc_ ? -1 : start_position_;
    }
    [[nodiscard]] int32_t end_position() const override;
    [[nodiscard]] int32_t width() const override { return input_->width(); }

    [[nodiscard]] VoidResult collect(SpanCollector &collector) override { return input_->collect(collector); }
    [[nodiscard]] float positions_cost() const override { return input_->positions_cost(); }

    [[nodiscard]] std::string to_string() const override;

protected:
    /// Decides about the current span of `candidate`, which is the wrapped spans.
    [[nodiscard]] virtual Result<SpanAcceptStatus> accept(Spans &candidate) = 0;

    [[nodiscard]] Spans &input() noexcept { return *input_; }

private:
    [[nodiscard]] Result<bool> two_phase_current_doc_matches();

    std::unique_ptr<Spans> input_;
    bool at_first_in_current_doc_{false};
    int32_t start_position_{-1};
};

} // namespace lucene
