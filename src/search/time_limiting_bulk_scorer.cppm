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

export module lucene_core:time_limiting_bulk_scorer;

import :types;
import std;

import :bulk_scorer;
import :error;
import :leaf_collector;
import :query_timeout;

namespace lucene {

/// Wraps a [`BulkScorer`] and aborts it once the [`QueryTimeout`] says the time is
/// up, reporting `kTimeExceeded` instead of a score.
///
/// Port of `TimeLimitingBulkScorer` (`core/search/time_limiting_bulk_scorer.rs`).
///
/// The wrapped scorer is driven in growing chunks (starting at [`kInterval`] and
/// growing by half each round) so that the timeout is checked often without
/// asking on every document.
export class TimeLimitingBulkScorer final : public BulkScorer {
public:
    /// How many documents the first chunk holds.
    static constexpr int32_t kInterval = 100;

    /// Wraps `scorer`, which must outlive this object, with `timeout`.
    TimeLimitingBulkScorer(std::unique_ptr<BulkScorer> scorer, const QueryTimeout &timeout);

    [[nodiscard]] Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                        int32_t max) override;

    [[nodiscard]] Result<int64_t> cost() override;

private:
    std::unique_ptr<BulkScorer> scorer_;
    const QueryTimeout *timeout_{nullptr};
};

} // namespace lucene
