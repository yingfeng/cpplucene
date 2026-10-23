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

export module lucene_core:field_value_hit_queue;

import :types;
import std;

import :error;
import :field_comparator;
import :sort_field;

namespace lucene {

/// One hit of a sorted search, as the queue holds it before the sort values are
/// copied out.
///
/// Port of `Entry` (`core/search/field_value_hit_queue.rs`).
export struct FieldValueEntry {
    /// The slot its sort values live in.
    int32_t slot{0};
    /// The global document number.
    int32_t doc{0};
    /// The score of the hit.
    float score{0.0F};
};

/// A heap of the `num_hits` best hits so far, with the least competitive hit at
/// the top so a new hit can replace it.
///
/// Port of the `FieldValueHitQueue` (`core/search/field_value_hit_queue.rs`).
///
/// **Deviation:** the reference implementation gets reverse ordering from a
/// reverse multiplier in its comparators; here the [`FieldComparator`]s already
/// apply it, so the queue compares their results directly.
export class FieldValueHitQueue {
public:
    /// A queue of `num_hits` slots sorted by `fields`, in priority order.
    [[nodiscard]] static Result<FieldValueHitQueue> create(std::vector<SortField> fields, int32_t num_hits);

    /// Pushes a hit whose sort values were already copied into `slot`.
    void push(FieldValueEntry entry);

    /// Replaces the document and score of the least competitive hit, whose sort
    /// values were copied into its slot, and restores the heap.
    void update_top(int32_t doc, float score);

    /// Records the current least competitive hit as the bottom for the
    /// comparators.
    void set_comparator_bottom();

    /// The least competitive hit.
    [[nodiscard]] const FieldValueEntry &top() const noexcept { return heap_.front(); }

    /// How many hits the queue holds.
    [[nodiscard]] size_t size() const noexcept { return heap_.size(); }
    /// How many hits the queue keeps.
    [[nodiscard]] int32_t num_hits() const noexcept { return num_hits_; }
    /// Whether the queue holds all the hits it can.
    [[nodiscard]] bool is_full() const noexcept { return heap_.size() >= static_cast<size_t>(num_hits_); }

    /// The hits, least competitive first; the queue is emptied.
    [[nodiscard]] std::vector<FieldValueEntry> take_all();

    /// The comparators, in the order of the sort fields.
    [[nodiscard]] std::vector<std::unique_ptr<FieldComparator>> &comparators() noexcept { return comparators_; }

    /// `true` when `left` sorts after `right`.
    [[nodiscard]] bool less_than(const FieldValueEntry &left, const FieldValueEntry &right) const;

private:
    FieldValueHitQueue(std::vector<std::unique_ptr<FieldComparator>> comparators, int32_t num_hits);

    std::vector<std::unique_ptr<FieldComparator>> comparators_;
    std::vector<FieldValueEntry> heap_;
    int32_t num_hits_{0};
};

} // namespace lucene
