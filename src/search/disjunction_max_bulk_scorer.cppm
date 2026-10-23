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

export module lucene_core:disjunction_max_bulk_scorer;

import :types;
import std;

import :bits;
import :bulk_scorer;
import :error;
import :fixed_bit_set;
import :leaf_collector;
import :priority_queue;
import :simple_scorable;

namespace lucene {

/// The width of the document window this scorer accumulates in.
inline constexpr size_t kDisjunctionMaxWindowSize = 4096;

/// Bulk scorer for a [`DisjunctionMaxQuery`](lucene_core:disjunction_max_query)
/// whose tie-break multiplier is zero.
///
/// Port of `DisjunctionMaxBulkScorer`
/// (`core/search/disjunction_max_bulk_scorer.rs`).
///
/// Every sub-scorer fills a window of documents; a document that several of them
/// match is collected once, with the maximum of the sub-scores.
///
/// **Deviation:** the reference implementation walks its heap slots directly;
/// here the queue stores indices into a shared entry array, as the rest of the
/// ported queue users do.
export class DisjunctionMaxBulkScorer final : public BulkScorer {
public:
    /// At least two sub-scorers are required.
    [[nodiscard]] static Result<std::unique_ptr<DisjunctionMaxBulkScorer>>
    create(std::vector<std::unique_ptr<BulkScorer>> scorers);

    [[nodiscard]] Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                        int32_t max) override;

    [[nodiscard]] Result<int64_t> cost() override;

private:
    /// A sub-scorer and the first document it may still match.
    struct Entry {
        std::unique_ptr<BulkScorer> scorer;
        int32_t next{0};
    };

    /// Orders the sub-scorers by the first document they may match.
    class EntryCompare {
    public:
        /// Shared, so that the address stays valid when the scorer is moved into
        /// its final owner.
        std::shared_ptr<const std::vector<Entry>> entries;

        [[nodiscard]] Result<bool> less_than(const size_t &left, const size_t &right) const {
            return (*entries)[left].next < (*entries)[right].next;
        }
    };

    explicit DisjunctionMaxBulkScorer(std::shared_ptr<std::vector<Entry>> entries,
                                      PriorityQueue<size_t, EntryCompare> queue);

    std::shared_ptr<std::vector<Entry>> entries_;
    FixedBitSet window_matches_;
    std::vector<float> window_scores_;
    PriorityQueue<size_t, EntryCompare> queue_;
    SimpleScorable top_level_scorable_;
};

} // namespace lucene
