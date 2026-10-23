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

export module lucene_core:disi_wrapper;

import :types;
import std;

import :doc_id_set_iterator;
import :error;
import :scorer;

namespace lucene {

/// A scorer plus the cached numbers the disjunction scorers keep about it: the
/// cost, the match cost and, for the max-score scorers, the scores of its window.
///
/// Port of `DisiWrapper` (`core/search/disi_wrapper.rs`).
///
/// The reference implementation uses it as a linked-list node (`next` is the
/// index of the following wrapper inside a scorer's array); the port keeps that
/// shape because the max-score scorers walk the list.
export template <class S> class DisiWrapper {
public:
    /// Wraps `scorer`, reading its cost and match cost.
    [[nodiscard]] static Result<DisiWrapper> create(S scorer) {
        auto cost = scorer.cost();
        if (!cost.has_value()) {
            return std::unexpected(std::move(cost.error()));
        }
        const float match_cost = scorer.match_cost();
        return DisiWrapper(std::move(scorer), cost.value(), match_cost);
    }

    /// The wrapped scorer.
    [[nodiscard]] S &scorer() noexcept { return scorer_; }
    [[nodiscard]] const S &scorer() const noexcept { return scorer_; }

    /// The index of the next wrapper in the owning scorer's list, if any.
    [[nodiscard]] std::optional<size_t> next() const noexcept { return next_; }
    void set_next(std::optional<size_t> next) noexcept { next_ = next; }

    /// The document the wrapper is positioned on.
    [[nodiscard]] int32_t doc() const noexcept { return doc_; }
    void set_doc(int32_t doc) noexcept { doc_ = doc; }

    /// The cost of the wrapped scorer, read when the wrapper was created.
    [[nodiscard]] int64_t cost() const noexcept { return cost_; }
    /// The match cost of the wrapped scorer, read when the wrapper was created.
    [[nodiscard]] float match_cost() const noexcept { return match_cost_; }

    /// The score of the current document scaled to an integer, for the
    /// max-score scorers.
    [[nodiscard]] int64_t scaled_max_score() const noexcept { return scaled_max_score_; }
    void set_scaled_max_score(int64_t score) noexcept { scaled_max_score_ = score; }
    /// The maximum score of the current window, for the max-score scorers.
    [[nodiscard]] float max_window_score() const noexcept { return max_window_score_; }
    void set_max_window_score(float score) noexcept { max_window_score_ = score; }

    /// Whether the current document really matches; fails when the scorer has no
    /// two-phase iterator.
    [[nodiscard]] Result<bool> matches() {
        if (scorer_.has_two_phase_iterator() == TwoPhaseState::No) {
            return err(LuceneError::illegal_state("this scorer does not support two-phase iteration"));
        }
        return scorer_.matches();
    }

    /// Whether the current document matches, tolerating a scorer without a
    /// two-phase iterator.
    [[nodiscard]] Result<bool> matches_may_none() { return scorer_.matches(); }

    /// Wraps `scorer` with the given cost and match cost, without reading them
    /// from it.
    ///
    /// **Deviation:** the reference implementation's constructor is private and
    /// `create` is the only way in; the port keeps the constructor reachable for
    /// callers whose scorer is a pointer or a `unique_ptr`, which can't be asked
    /// for its cost through `create`.
    DisiWrapper(S scorer, int64_t cost, float match_cost)
            : scorer_(std::move(scorer)), cost_(cost), match_cost_(match_cost) {}

private:
    S scorer_;
    std::optional<size_t> next_{};
    int32_t doc_{-1};
    int64_t cost_{0};
    float match_cost_{0.0F};
    int64_t scaled_max_score_{0};
    float max_window_score_{0.0F};
};

} // namespace lucene
