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

export module lucene_core:scorer;

import :types;
import std;

import :doc_id_set_iterator;
import :error;
import :scorable;

namespace lucene {

/// Whether a scorer needs a confirmation pass.
///
/// Port of `TwoPhaseState` (`core/search/scorer.rs`).
export enum class TwoPhaseState : int32_t {
    /// `matches()` must be called for every candidate.
    Yes = 0,
    /// Every candidate matches.
    No = 1,
};

/// Common scoring functionality: a [`DocIdSetIterator`] over the documents that
/// fully match, plus their scores.
///
/// Port of `Scorer` (`core/search/scorer.rs`).
///
/// **Deviation:** the reference implementation builds `Scorer` out of the `Scorable`,
/// `DocIdSetIterator` and `TwoPhaseIterator` traits (three separate objects with
/// an `Either` enum for the combinations); here the three are one class
/// hierarchy — `Scorer` derives from [`DocIdSetIterator`] and [`Scorable`] and
/// carries the two-phase entry points itself, with defaults that forward to the
/// full-matching cursor so scorers without an approximation need no extra code.
export class Scorer : public DocIdSetIterator, public Scorable {
public:
    /// The document currently being scored; differs from [`doc_id`] only for
    /// scorers whose scoring cursor lags the matching cursor.
    [[nodiscard]] virtual Result<int32_t> scoring_doc_id() = 0;

    /// Moves to the block that contains `target` without changing
    /// [`doc_id`], and returns the last document of that block.
    [[nodiscard]] virtual Result<int32_t> advance_shallow(int32_t /*target*/) { return NO_MORE_DOCS; }

    /// The default of [`advance_shallow`], for wrappers that forward to it.
    [[nodiscard]] virtual Result<int32_t> default_advance_shallow(int32_t /*target*/) { return NO_MORE_DOCS; }

    /// The maximum score of the documents in `[last advance_shallow target, upto]`.
    [[nodiscard]] virtual Result<float> get_max_score(int32_t upto) = 0;

    /// The cost reported through [`DocIdSetIterator::cost`], for scorers that
    /// wrap another iterator.
    [[nodiscard]] virtual Result<int64_t> default_cost() { return DocIdSetIterator::cost(); }

    /// Whether [`matches`] has to be called.
    [[nodiscard]] virtual TwoPhaseState has_two_phase_iterator() const = 0;

    // ---- two-phase entry points; the defaults mean "no approximation" ----

    /// Whether the current candidate really matches.  Defaults to `true`.
    [[nodiscard]] virtual Result<bool> matches() { return true; }

    /// The expected cost of one [`matches`] call.  Defaults to `0`.
    [[nodiscard]] virtual float match_cost() const { return 0.0F; }

    /// The candidate cursor; defaults to the full-matching cursor.
    [[nodiscard]] virtual int32_t approx_doc_id() const { return doc_id(); }
    [[nodiscard]] virtual Result<int32_t> approx_next_doc() { return next_doc(); }
    [[nodiscard]] virtual Result<int32_t> approx_advance(int32_t target) { return advance(target); }
    [[nodiscard]] virtual Result<int64_t> approx_cost() const { return cost(); }
};

} // namespace lucene
