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

export module lucene_core:hit_queue;

import :types;

import std;

import :error;
import :priority_queue;
import :score_doc;

namespace lucene {

/// Orders hits so that the least competitive one sits on top of the queue:
/// lower scores first, and among equal scores the higher document number,
/// because hits are collected in increasing document order.
///
/// Port of `HitQueueComparator` (`core/search/hit_queue.rs`).
export struct HitQueueComparator {
    [[nodiscard]] Result<bool> less_than(const ScoreDoc &left, const ScoreDoc &right) const {
        if (left.score == right.score) {
            return left.doc > right.doc;
        }
        return left.score < right.score;
    }
};

/// The queue of the top-scoring hits.
export using HitQueue = PriorityQueue<ScoreDoc, HitQueueComparator>;

/// Creates a hit queue of `size` elements.
///
/// When `pre_populate` is set, the queue starts full of sentinel hits
/// (`doc = INT32_MAX`, `score = -infinity`) so the collector can always replace
/// the top instead of inserting.
export [[nodiscard]] inline Result<HitQueue> new_hit_queue(size_t size, bool pre_populate) {
    if (pre_populate) {
        return HitQueue::with_sentinel_object(
                size,
                [] -> std::optional<ScoreDoc> {
                    return ScoreDoc::create(std::numeric_limits<int32_t>::max(),
                                            -std::numeric_limits<float>::infinity());
                },
                HitQueueComparator{});
    }
    return HitQueue::create(size, HitQueueComparator{});
}

} // namespace lucene
