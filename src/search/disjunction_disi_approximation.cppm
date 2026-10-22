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

export module lucene_core:disjunction_disi_approximation;

import :types;

import std;

import :doc_id_set_iterator;
import :error;
import :scorer;

namespace lucene {

/// Iterator over the union of several sub-iterators, keeping the least
/// positioned one at the head of a min-heap.
///
/// Port of `DisjunctionDISIApproximation`
/// (`core/search/disjunction_disi_approximation.rs`).
///
/// **Deviation:** the sub-scorers are borrowed (`Scorer *`), not owned, exactly
/// like rlucene's `DisiWrapper` array; the owning scorer
/// ([`DisjunctionSumScorer`](lucene_core:disjunction_scorer)) keeps them alive.
/// The bit set fast path (`BitSetIterator` sub-iterators) is not ported.
export class DisjunctionDISIApproximation final : public DocIdSetIterator {
public:
    /// `sub_scorers` must hold at least two scorers, all positioned on the same
    /// document.
    [[nodiscard]] static Result<std::unique_ptr<DisjunctionDISIApproximation>>
    create(const std::vector<Scorer *> &sub_scorers);

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    /// The sub-scorers; the head of the heap is `sub_scorers().front()`.
    [[nodiscard]] const std::vector<Scorer *> &sub_scorers() const noexcept { return heap_; }

private:
    explicit DisjunctionDISIApproximation(std::vector<Scorer *> heap) : heap_(std::move(heap)) {}

    /// Restores the heap after the head advanced.
    void update_top();

    /// The sub-scorers, arranged as a min-heap on `doc_id()`.
    std::vector<Scorer *> heap_;
};

} // namespace lucene
