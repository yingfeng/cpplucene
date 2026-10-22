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

export module lucene_core:top_docs;

import :types;

import std;

import :error;
import :priority_queue;
import :score_doc;
import :total_hits;

namespace lucene {

/// The hits a search returned.
///
/// Port of `TopDocs` (`core/search/top_docs.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * rlucene's `TopDocs<S>` is generic over `ScoreDocLike`; the port keeps one
///    `TopDocs` over [`ScoreDoc`] until the sorting layer (which is what needs
///    `TopFieldScoreDoc`) is ported, and `TopDocsLike` disappears with it;
///  * `merge_top_field_docs*` / `MergeSortQueueCmp` are not ported: they sort by
///    `Sort` and compare field values, neither of which exists yet.
export struct TopDocs {
    /// The total number of hits of the query.
    TotalHits total_hits;
    /// The top hits, most competitive first.
    std::vector<ScoreDoc> score_docs;

    [[nodiscard]] static TopDocs create(TotalHits total_hits, std::vector<ScoreDoc> score_docs) {
        return TopDocs{std::move(total_hits), std::move(score_docs)};
    }
};

/// Which shard and which hit within it the merge queue is positioned on.
///
/// Port of `ShardRef` (`core/search/top_docs.rs`).
export struct ShardRef {
    /// Index into the array of shard results.
    size_t shard_index{0};
    /// Index of the hit within that shard.
    size_t hit_index{0};

    [[nodiscard]] static ShardRef create(size_t shard_index) { return ShardRef{shard_index, 0}; }

    [[nodiscard]] std::string to_string() const {
        return "ShardRef(shardIndex=" + std::to_string(shard_index) + " hitIndex=" + std::to_string(hit_index) + ")";
    }
};

/// Breaks ties on the shard index; the innermost tie breaker of
/// [`DefaultTieBreaker`].
export struct ShardIndexTieBreaker {
    [[nodiscard]] Result<int32_t> compare(const ScoreDoc &left, const ScoreDoc &right) const {
        if (left.shard_index < right.shard_index) {
            return -1;
        }
        return left.shard_index > right.shard_index ? 1 : 0;
    }
};

/// Breaks ties on the document number.
export struct DocIdTieBreaker {
    [[nodiscard]] Result<int32_t> compare(const ScoreDoc &left, const ScoreDoc &right) const {
        if (left.doc < right.doc) {
            return -1;
        }
        return left.doc > right.doc ? 1 : 0;
    }
};

/// The default tie breaker: shard index first, then document number.
export struct DefaultTieBreaker {
    [[nodiscard]] Result<int32_t> compare(const ScoreDoc &left, const ScoreDoc &right) const {
        auto by_shard = shard_compare.compare(left, right);
        if (!by_shard.has_value()) {
            return by_shard;
        }
        if (by_shard.value() != 0) {
            return by_shard;
        }
        return doc_compare.compare(left, right);
    }

    ShardIndexTieBreaker shard_compare{};
    DocIdTieBreaker doc_compare{};
};

/// Uses the tie breaker when it gives an answer, and the intra-shard hit order
/// otherwise.
///
/// Port of `tie_break_less_than` (`core/search/top_docs.rs`).
export template <class C>
[[nodiscard]] Result<bool> tie_break_less_than(const ShardRef &first, const ScoreDoc &first_doc, const ShardRef &second,
                                               const ScoreDoc &second_doc, const C &tie_breaker) {
    auto compared = tie_breaker.compare(first_doc, second_doc);
    if (!compared.has_value()) {
        return std::unexpected(std::move(compared.error()));
    }
    if (compared.value() == 0) {
        // Equal values: the shard already resolved this order, keep it.
        return first.hit_index < second.hit_index;
    }
    return compared.value() < 0;
}

/// Orders the shards of a score-sorted merge by the score of their current hit.
///
/// Port of `ScoreMergeSortQueueCmp` (`core/search/top_docs.rs`).
export template <class C>
class ScoreMergeSortQueueCmp {
public:
    ScoreMergeSortQueueCmp(const std::vector<TopDocs> *shard_hits, C tie_breaker)
        : shard_hits_(shard_hits), tie_breaker_(std::move(tie_breaker)) {}

    [[nodiscard]] Result<bool> less_than(const ShardRef &first, const ShardRef &second) const {
        const ScoreDoc &first_doc = (*shard_hits_)[first.shard_index].score_docs[first.hit_index];
        const ScoreDoc &second_doc = (*shard_hits_)[second.shard_index].score_docs[second.hit_index];
        if (first_doc.score < second_doc.score) {
            return false;
        }
        if (first_doc.score > second_doc.score) {
            return true;
        }
        return tie_break_less_than(first, first_doc, second, second_doc, tie_breaker_);
    }

private:
    const std::vector<TopDocs> *shard_hits_;
    C tie_breaker_;
};

namespace detail {

/// The merge loop shared by every `merge_top_docs*`: pops the most competitive
/// shard hit at a time, skipping the first `start` hits.
///
/// Port of `merge_aux` (`core/search/top_docs.rs`).
template <class Cmp>
[[nodiscard]] Result<TopDocs> merge_aux(PriorityQueue<ShardRef, Cmp> &queue, size_t start, size_t size,
                                        const std::vector<TopDocs> &shard_hits) {
    int64_t total_hit_count = 0;
    TotalHits::Relation total_hits_relation = TotalHits::Relation::EqualTo;
    size_t avail_hit_count = 0;

    for (size_t shard_index = 0; shard_index < shard_hits.size(); ++shard_index) {
        const TopDocs &shard = shard_hits[shard_index];
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

    std::vector<ScoreDoc> hits;
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

            const TopDocs &shard = shard_hits[ref->shard_index];
            if (ref->hit_index >= shard.score_docs.size()) {
                return err(LuceneError::illegal_state("shard hit index out of range"));
            }
            const ScoreDoc &hit = shard.score_docs[ref->hit_index];
            ++ref->hit_index;

            // The shard indices must be consistent: either every hit carries
            // one or none does, which defends against caller bugs.
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
    return TopDocs::create(TotalHits::create(static_cast<size_t>(total_hit_count), total_hits_relation),
                           std::move(hits));
}

} // namespace detail

/// Merges score-sorted shard results with a custom tie breaker, skipping the
/// first `start` hits.
///
/// Port of `merge_top_docs_with_comparator` (`core/search/top_docs.rs`).
export template <class C>
[[nodiscard]] Result<TopDocs> merge_top_docs_with_comparator(size_t start, size_t size,
                                                            const std::vector<TopDocs> &shard_hits, C tie_breaker) {
    ScoreMergeSortQueueCmp<C> cmp(&shard_hits, std::move(tie_breaker));
    auto queue = PriorityQueue<ShardRef, ScoreMergeSortQueueCmp<C>>::create(shard_hits.size(), std::move(cmp));
    if (!queue.has_value()) {
        return std::unexpected(std::move(queue.error()));
    }
    return detail::merge_aux(queue.value(), start, size, shard_hits);
}

/// Merges score-sorted shard results, skipping the first `start` hits.
///
/// Port of `merge_top_docs_with_start` (`core/search/top_docs.rs`).
export [[nodiscard]] inline Result<TopDocs> merge_top_docs_with_start(size_t start, size_t top_n,
                                                                     const std::vector<TopDocs> &shard_hits) {
    return merge_top_docs_with_comparator(start, top_n, shard_hits, DefaultTieBreaker{});
}

/// Merges score-sorted shard results, returning the top `top_n` hits.
///
/// Port of `merge_top_docs` (`core/search/top_docs.rs`).
export [[nodiscard]] inline Result<TopDocs> merge_top_docs(size_t top_n, const std::vector<TopDocs> &shard_hits) {
    return merge_top_docs_with_start(0, top_n, shard_hits);
}

} // namespace lucene
