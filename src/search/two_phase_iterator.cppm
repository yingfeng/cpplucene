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

export module lucene_core:two_phase_iterator;

import :types;
import std;

import :doc_id_set_iterator;
import :error;

namespace lucene {

/// Candidate advancement plus a confirmation step, on one cursor.
///
/// Port of `TwoPhaseIterator` (`core/search/two_phase_iterator.rs`).
///
/// The `approx_*` methods may stop on documents that [`matches`] rejects;
/// scorers that need no confirmation return `true` and `0.0F`.
export class TwoPhaseIterator {
public:
    virtual ~TwoPhaseIterator() = default;

    /// The current candidate document (`-1` before the first advance,
    /// [`NO_MORE_DOCS`] when exhausted).
    [[nodiscard]] virtual int32_t approx_doc_id() const = 0;

    [[nodiscard]] virtual Result<int32_t> approx_next_doc() = 0;
    [[nodiscard]] virtual Result<int32_t> approx_advance(int32_t target) = 0;

    /// Linear fallback for [`approx_advance`]; the default calls
    /// [`approx_next_doc`] until it is at or past `target`.
    [[nodiscard]] virtual Result<int32_t> approx_slow_advance(int32_t target);

    [[nodiscard]] virtual Result<int64_t> approx_cost() const = 0;

    /// Whether the current candidate really matches.
    [[nodiscard]] virtual Result<bool> matches() = 0;

    /// The expected cost of one [`matches`] call.
    [[nodiscard]] virtual float match_cost() const = 0;
};

/// Presents a [`TwoPhaseIterator`] as a [`DocIdSetIterator`], skipping the
/// candidates that [`matches`] rejects.
///
/// Port of `TwoPhaseIteratorAsDocIdSetIterator`
/// (`core/search/two_phase_iterator.rs`).
export class TwoPhaseIteratorAsDocIdSetIterator final : public DocIdSetIterator {
public:
    explicit TwoPhaseIteratorAsDocIdSetIterator(std::unique_ptr<TwoPhaseIterator> two_phase);

    [[nodiscard]] int32_t doc_id() const override { return two_phase_->approx_doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return two_phase_->approx_cost(); }

    /// The wrapped iterator.
    [[nodiscard]] TwoPhaseIterator &two_phase_iterator() const noexcept { return *two_phase_; }

private:
    /// Advances until a candidate is confirmed or the iterator is exhausted.
    [[nodiscard]] Result<int32_t> do_next(int32_t doc);

    std::unique_ptr<TwoPhaseIterator> two_phase_;
};

/// Wraps `two_phase` into a [`DocIdSetIterator`].
[[nodiscard]] std::unique_ptr<DocIdSetIterator> as_doc_id_set_iterator(
        std::unique_ptr<TwoPhaseIterator> two_phase);

} // namespace lucene
