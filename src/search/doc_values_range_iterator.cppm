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

export module lucene_core:doc_values_range_iterator;

import :types;
import std;

import :doc_values_skipper;
import :error;
import :two_phase_iterator;

namespace lucene {

/// What a skipper interval proves about the range.
///
/// Port of `Match` (`core/search/doc_values_range_iterator.rs`).
export enum class Match {
    /// No document of the interval matches.
    kNo,
    /// The values have to be checked to tell.
    kMaybe,
    /// Every document of the interval that has a value matches.
    kIfDocHasValue,
    /// Every document of the interval matches.
    kYes,
};

/// A [`TwoPhaseIterator`] that skips doc values intervals whose values all fall
/// outside the requested range.
///
/// Port of `DocValuesRangeIterator`
/// (`core/search/doc_values_range_iterator.rs`).
///
/// **Deviation:** the reference implementation also exposes the approximation
/// through `ApproximationDISI`/`ApproximationBitSetAccess`; here the
/// [`TwoPhaseIterator`] already carries the approximation cursor, so those two
/// traits collapse into it.
export class DocValuesRangeIterator final : public TwoPhaseIterator {
public:
    /// `inner` is the approximation of the range query, `skipper` the doc values
    /// skipper of the field, and `has_gaps` tells whether documents without a
    /// value may sit inside an interval.
    DocValuesRangeIterator(std::unique_ptr<TwoPhaseIterator> inner, std::shared_ptr<DocValuesSkipper> skipper,
                           int64_t lower_value, int64_t upper_value, bool has_gaps);

    // ---- TwoPhaseIterator ----

    [[nodiscard]] int32_t approx_doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> approx_next_doc() override { return approx_advance(doc_ + 1); }
    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> approx_cost() const override;

    [[nodiscard]] Result<bool> matches() override;

    [[nodiscard]] float match_cost() const override { return inner_->match_cost(); }

    /// The underlying approximation.
    [[nodiscard]] TwoPhaseIterator &inner() noexcept { return *inner_; }

    /// The decision for the current interval, for diagnostics.
    [[nodiscard]] Match match_state() const noexcept { return match_; }

    /// The last document of the interval the iterator is in.
    [[nodiscard]] int32_t upto() const noexcept { return upto_; }

private:
    /// The decision for the interval at `level`.
    [[nodiscard]] Result<Match> match_at_level(size_t level) const;

    std::unique_ptr<TwoPhaseIterator> inner_;
    std::shared_ptr<DocValuesSkipper> skipper_;
    int64_t lower_value_;
    int64_t upper_value_;
    bool has_gaps_;
    int32_t doc_{-1};
    Match match_{Match::kMaybe};
    int32_t upto_{-1};
};

} // namespace lucene
