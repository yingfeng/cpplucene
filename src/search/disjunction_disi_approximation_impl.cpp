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

module lucene_core:disjunction_disi_approximation.impl;

import :disjunction_disi_approximation;

import :doc_id_set_iterator;
import :error;
import :scorer;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<DisjunctionDISIApproximation>>
DisjunctionDISIApproximation::create(const std::vector<Scorer *> &sub_scorers) {
    if (sub_scorers.size() < 2) {
        return err(LuceneError::illegal_argument("There must be at least 2 sub-scorers, got " +
                                                 std::to_string(sub_scorers.size())));
    }
    const int32_t cur_doc = sub_scorers[0]->doc_id();
    for (const auto *scorer : sub_scorers) {
        if (scorer == nullptr) {
            return err(LuceneError::illegal_argument("DisjunctionDISIApproximation sub-scorers must not be null"));
        }
        if (scorer->doc_id() != cur_doc) {
            return err(LuceneError::illegal_argument(
                    "Sub-iterators of DisjunctionDISIApproximation are not on the same document!"));
        }
    }
    auto approximation =
            std::unique_ptr<DisjunctionDISIApproximation>(new DisjunctionDISIApproximation(sub_scorers));
    // The sub-scorers all sit on the same document, so the array is already a
    // valid heap.
    return approximation;
}

int32_t DisjunctionDISIApproximation::doc_id() const { return heap_.front()->doc_id(); }

Result<int32_t> DisjunctionDISIApproximation::next_doc() {
    const int32_t old_doc = doc_id();
    // Several sub-iterators may share the current document; keep advancing the
    // head until the document actually changes.  Exhausted iterators simply sink
    // to the bottom of the heap.
    for (;;) {
        auto advanced = heap_.front()->next_doc();
        if (!advanced.has_value()) {
            return advanced;
        }
        update_top();
        if (heap_.front()->doc_id() != old_doc) {
            return heap_.front()->doc_id();
        }
    }
}

Result<int32_t> DisjunctionDISIApproximation::advance(int32_t target) {
    // Same loop as `next_doc`, but every sub-iterator has to reach `target`.
    for (;;) {
        auto advanced = heap_.front()->advance(target);
        if (!advanced.has_value()) {
            return advanced;
        }
        update_top();
        if (heap_.front()->doc_id() >= target) {
            return heap_.front()->doc_id();
        }
    }
}

Result<int64_t> DisjunctionDISIApproximation::cost() const {
    int64_t total = 0;
    for (const auto *scorer : heap_) {
        auto sub_cost = scorer->cost();
        if (!sub_cost.has_value()) {
            return std::unexpected(std::move(sub_cost.error()));
        }
        total += sub_cost.value();
    }
    return total;
}

void DisjunctionDISIApproximation::update_top() {
    // Sift the new head down: every other element was already in heap order.
    size_t index = 0;
    const size_t size = heap_.size();
    for (;;) {
        const size_t left = (2 * index) + 1;
        if (left >= size) {
            return;
        }
        size_t smallest = left;
        const size_t right = left + 1;
        if (right < size && heap_[right]->doc_id() < heap_[left]->doc_id()) {
            smallest = right;
        }
        if (heap_[index]->doc_id() <= heap_[smallest]->doc_id()) {
            return;
        }
        std::swap(heap_[index], heap_[smallest]);
        index = smallest;
    }
}

} // namespace lucene
