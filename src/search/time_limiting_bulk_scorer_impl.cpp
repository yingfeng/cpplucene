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

module lucene_core:time_limiting_bulk_scorer.impl;

import :time_limiting_bulk_scorer;

import std;

import :bulk_scorer;
import :error;
import :leaf_collector;
import :query_timeout;
import :types;

namespace lucene {

TimeLimitingBulkScorer::TimeLimitingBulkScorer(std::unique_ptr<BulkScorer> scorer, const QueryTimeout &timeout)
        : scorer_(std::move(scorer)), timeout_(&timeout) {}

Result<int32_t> TimeLimitingBulkScorer::score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                              int32_t max) {
    int32_t interval = kInterval;
    while (min < max) {
        const int32_t new_max = static_cast<int32_t>(std::min(static_cast<int64_t>(min) + interval,
                                                             static_cast<int64_t>(max)));
        const int32_t new_interval = interval + (interval >> 1);
        if (interval < new_interval) {
            interval = new_interval;
        }
        if (timeout_->should_exit()) {
            return err(LuceneError::time_exceeded("search exceeded its time budget"));
        }
        auto scored = scorer_->score(collector, accept_docs, min, new_max);
        if (!scored.has_value()) {
            return std::unexpected(std::move(scored.error()));
        }
        min = scored.value();
    }
    return min;
}

Result<int64_t> TimeLimitingBulkScorer::cost() { return scorer_->cost(); }

} // namespace lucene
