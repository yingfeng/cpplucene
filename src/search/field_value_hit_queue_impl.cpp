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

module lucene_core:field_value_hit_queue.impl;

import :field_value_hit_queue;

import std;

import :error;
import :field_comparator;
import :sort_field;
import :types;

namespace lucene {

FieldValueHitQueue::FieldValueHitQueue(std::vector<std::unique_ptr<FieldComparator>> comparators, int32_t num_hits)
        : comparators_(std::move(comparators)), num_hits_(num_hits) {}

Result<FieldValueHitQueue> FieldValueHitQueue::create(std::vector<SortField> fields, int32_t num_hits) {
    if (fields.empty()) {
        return err(LuceneError::illegal_argument("Sort must contain at least one field"));
    }
    if (num_hits <= 0) {
        return err(LuceneError::illegal_argument("numHits must be > 0; please use TotalHitCountCollector if you "
                                                "only need the total hit count"));
    }
    std::vector<std::unique_ptr<FieldComparator>> comparators;
    comparators.reserve(fields.size());
    for (const SortField &field : fields) {
        auto comparator = comparator_for(field, num_hits);
        if (!comparator.has_value()) {
            return std::unexpected(std::move(comparator.error()));
        }
        comparators.push_back(std::move(comparator.value()));
    }
    return FieldValueHitQueue(std::move(comparators), num_hits);
}

bool FieldValueHitQueue::less_than(const FieldValueEntry &left, const FieldValueEntry &right) const {
    for (const std::unique_ptr<FieldComparator> &comparator : comparators_) {
        const int32_t result = comparator->compare(left.slot, right.slot);
        if (result != 0) {
            return result > 0;
        }
    }
    // Documents are collected in increasing order, so the larger document is the
    // less competitive hit.
    return left.doc > right.doc;
}

void FieldValueHitQueue::push(FieldValueEntry entry) {
    heap_.push_back(entry);
    // The front has to stay the least competitive hit: the heap orders by "sorts
    // before", so the element that sorts before nothing else is on top.
    std::push_heap(heap_.begin(), heap_.end(),
                   [this](const FieldValueEntry &left, const FieldValueEntry &right) {
                       return less_than(right, left);
                   });
}

void FieldValueHitQueue::update_top(int32_t doc, float score) {
    heap_.front().doc = doc;
    heap_.front().score = score;
    // The front changed: take it out and put it back, which sifts it down.
    std::pop_heap(heap_.begin(), heap_.end(),
                  [this](const FieldValueEntry &left, const FieldValueEntry &right) {
                      return less_than(right, left);
                  });
    std::push_heap(heap_.begin(), heap_.end(),
                   [this](const FieldValueEntry &left, const FieldValueEntry &right) {
                       return less_than(right, left);
                   });
}

void FieldValueHitQueue::set_comparator_bottom() {
    if (heap_.empty()) {
        return;
    }
    const int32_t slot = heap_.front().slot;
    for (std::unique_ptr<FieldComparator> &comparator : comparators_) {
        comparator->set_bottom(slot);
    }
}

std::vector<FieldValueEntry> FieldValueHitQueue::take_all() {
    std::vector<FieldValueEntry> entries;
    entries.reserve(heap_.size());
    while (!heap_.empty()) {
        std::pop_heap(heap_.begin(), heap_.end(),
                      [this](const FieldValueEntry &left, const FieldValueEntry &right) {
                          return less_than(right, left);
                      });
        entries.push_back(heap_.back());
        heap_.pop_back();
    }
    return entries;
}

} // namespace lucene
