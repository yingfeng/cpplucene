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

module lucene_core:top_field_docs.impl;

import :top_field_docs;

import std;

import :error;
import :field_comparator;
import :priority_queue;
import :sort_field;
import :top_docs;
import :types;

namespace lucene {

namespace {

/// Orders the merge queue by the sort values of the head hit of each shard.
///
/// Port of `MergeSortQueueCmp` (`core/search/top_docs.rs`).
struct FieldMergeSortQueueCmp {
    const std::vector<TopFieldDocs> *shard_hits{nullptr};
    /// The comparators of the sort fields; they already apply
    /// [`reverse`](SortField::reverse), which is why there is no reverse
    /// multiplier here (the reference implementation keeps the natural order and
    /// multiplies by -1 for a reversed field).
    std::vector<std::unique_ptr<FieldComparator>> comparators;
    DefaultTieBreaker tie_breaker{};

    [[nodiscard]] Result<bool> less_than(const ShardRef &first, const ShardRef &second) const {
        const FieldDoc &first_hit = (*shard_hits)[first.shard_index].score_docs[first.hit_index];
        const FieldDoc &second_hit = (*shard_hits)[second.shard_index].score_docs[second.hit_index];
        for (size_t i = 0; i < comparators.size(); ++i) {
            auto compared = comparators[i]->compare_values(first_hit.fields[i], second_hit.fields[i]);
            if (!compared.has_value()) {
                return std::unexpected(std::move(compared.error()));
            }
            if (compared.value() != 0) {
                return compared.value() < 0;
            }
        }
        return tie_break_less_than(first, first_hit, second, second_hit, tie_breaker);
    }
};

/// The merge loop over sorted hits: pops the most competitive shard hit at a
/// time, skipping the first `start` hits.
///
/// Port of `merge_aux` (`core/search/top_docs.rs`) for `TopFieldDocs`.
[[nodiscard]] Result<TopFieldDocs> merge_field_aux(PriorityQueue<ShardRef, FieldMergeSortQueueCmp> &queue,
                                                   const Sort &sort, size_t start, size_t size,
                                                   const std::vector<TopFieldDocs> &shard_hits) {
    int64_t total_hit_count = 0;
    TotalHits::Relation total_hits_relation = TotalHits::Relation::EqualTo;
    size_t avail_hit_count = 0;

    for (size_t shard_index = 0; shard_index < shard_hits.size(); ++shard_index) {
        const TopFieldDocs &shard = shard_hits[shard_index];
        total_hit_count += static_cast<int64_t>(shard.total_hits.value);
        if (shard.total_hits.relation == TotalHits::Relation::GreaterThanOrEqualTo) {
            total_hits_relation = TotalHits::Relation::GreaterThanOrEqualTo;
        }
        if (!shard.score_docs.empty()) {
            avail_hit_count += shard.score_docs.size();
            if (auto added = queue.add(ShardRef::create(shard_index)); !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
        }
    }

    std::vector<FieldDoc> hits;
    if (avail_hit_count > start) {
        const size_t result_len = std::min(size, avail_hit_count - start);
        hits.resize(result_len);

        const size_t requested_result_window = start + size;
        const size_t num_iter_on_hits = std::min(avail_hit_count, requested_result_window);
        size_t hit_upto = 0;
        bool unset_shard_index = false;

        while (hit_upto < num_iter_on_hits) {
            ShardRef *ref = queue.top_mut();
            if (ref == nullptr) {
                return err(LuceneError::illegal_state("priority queue is empty"));
            }

            const TopFieldDocs &shard = shard_hits[ref->shard_index];
            if (ref->hit_index >= shard.score_docs.size()) {
                return err(LuceneError::illegal_state("shard hit index out of range"));
            }
            const FieldDoc &hit = shard.score_docs[ref->hit_index];
            ++ref->hit_index;

            // The shard indices must be consistent: either every hit carries one
            // or none does, which defends against caller bugs.
            if (hit_upto > 0 && unset_shard_index != (hit.shard_index == -1)) {
                return err(LuceneError::illegal_argument("Inconsistent order of shard indices"));
            }
            unset_shard_index = unset_shard_index || hit.shard_index == -1;

            if (hit_upto >= start) {
                hits[hit_upto - start] = hit;
            }
            ++hit_upto;

            if (ref->hit_index < shard.score_docs.size()) {
                if (auto updated = queue.update_top(); !updated.has_value()) {
                    return std::unexpected(std::move(updated.error()));
                }
            } else if (auto popped = queue.pop_unchecked(); !popped.has_value()) {
                return std::unexpected(std::move(popped.error()));
            }
        }
    }

    if (total_hit_count < 0) {
        return err(LuceneError::illegal_argument("value must be >= 0, got " + std::to_string(total_hit_count)));
    }
    return TopFieldDocs(TotalHits::create(static_cast<size_t>(total_hit_count), total_hits_relation), std::move(hits),
                        sort);
}

} // namespace

Result<TopFieldDocs> merge_top_field_docs_with_start(const Sort &sort, size_t start, size_t top_n,
                                                     const std::vector<TopFieldDocs> &shard_hits) {
    const std::vector<SortField> &fields = sort.fields();
    FieldMergeSortQueueCmp cmp;
    cmp.shard_hits = &shard_hits;
    cmp.comparators.reserve(fields.size());

    // A hit has to carry one sort value per field, otherwise the merge would
    // compare values of a search that was not sorted by this sort.
    for (size_t shard_index = 0; shard_index < shard_hits.size(); ++shard_index) {
        for (const FieldDoc &hit : shard_hits[shard_index].score_docs) {
            if (hit.fields.size() != fields.size()) {
                return err(LuceneError::illegal_argument(
                        "shard " + std::to_string(shard_index) +
                        " was not sorted by the provided Sort (expected " + std::to_string(fields.size()) +
                        " sort values per hit, got " + std::to_string(hit.fields.size()) + ")"));
            }
        }
    }
    for (const SortField &field : fields) {
        auto comparator = comparator_for(field, 1);
        if (!comparator.has_value()) {
            return std::unexpected(std::move(comparator.error()));
        }
        cmp.comparators.push_back(std::move(comparator.value()));
    }

    auto queue =
            PriorityQueue<ShardRef, FieldMergeSortQueueCmp>::create(shard_hits.size(), std::move(cmp));
    if (!queue.has_value()) {
        return std::unexpected(std::move(queue.error()));
    }
    return merge_field_aux(queue.value(), sort, start, top_n, shard_hits);
}

Result<TopFieldDocs> merge_top_field_docs(const Sort &sort, size_t top_n, const std::vector<TopFieldDocs> &shard_hits) {
    return merge_top_field_docs_with_start(sort, 0, top_n, shard_hits);
}

} // namespace lucene
