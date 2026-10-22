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

export module lucene_core:bulk_scorer;

import :types;
import std;

import :bits;
import :doc_id_set_iterator;
import :error;
import :leaf_collector;
import :scorer;

namespace lucene {

/// Scores a range of documents at once.
///
/// Port of the `BulkScorer` trait (`core/search/bulk_scorer.rs`).
///
/// Queries that can score more efficiently than by iterating their scorer
/// provide their own implementation; everything else uses
/// [`DefaultBulkScorer`].
export class BulkScorer {
public:
    virtual ~BulkScorer() = default;

    /// Collects the matches in `[min, max)` and returns an estimation of the
    /// next matching document at or after `max` ([`NO_MORE_DOCS`] when there is
    /// none).
    ///
    /// `accept_docs` restricts the documents that may match; `nullptr` accepts
    /// everything.
    [[nodiscard]] virtual Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                                int32_t max) = 0;

    /// The same as [`DocIdSetIterator::cost`], for bulk scorers.
    [[nodiscard]] virtual Result<int64_t> cost() = 0;
};

/// The generic [`BulkScorer`]: iterates a [`Scorer`] and feeds a collector.
///
/// Port of `DefaultBulkScorer` (`core/search/weight.rs`).
export class DefaultBulkScorer final : public BulkScorer {
public:
    explicit DefaultBulkScorer(std::unique_ptr<Scorer> scorer);

    [[nodiscard]] Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                        int32_t max) override;

    [[nodiscard]] Result<int64_t> cost() override;

private:
    std::unique_ptr<Scorer> scorer_;
};

} // namespace lucene
