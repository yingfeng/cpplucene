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

export module lucene_core:scorer_util;

import :types;
import std;

import :error;

namespace lucene {

/// Helpers the scorer suppliers share.
///
/// Port of `ScorerUtil` (`core/search/scorer_util.rs`).
export class ScorerUtil {
public:
    /// The cost of a disjunction of `num_scorers` clauses of which
    /// `min_should_match` have to match.
    ///
    /// A `MUST`-`SHOULD` query with `min_should_match = m` is the sum of the costs
    /// of the `num_scorers - m + 1` cheapest clauses, because that is how many
    /// clauses a disjunction may have to be evaluated before `m` of them matched.
    ///
    /// **Deviation:** the reference implementation takes an iterator and has a
    /// size-hint fast path; here the costs are already materialised.
    [[nodiscard]] static Result<int64_t> cost_with_min_should_match(std::span<const int64_t> costs,
                                                                    size_t num_scorers, size_t min_should_match) {
        if (num_scorers < min_should_match) {
            return err(LuceneError::illegal_argument(std::format(
                    "min_should_match must be <= num_scorers, got {} and {}", min_should_match, num_scorers)));
        }
        const size_t keep = num_scorers - min_should_match + 1;
        if (keep == 1) {
            int64_t lowest = std::numeric_limits<int64_t>::max();
            for (const int64_t cost : costs) {
                lowest = std::min(lowest, cost);
            }
            return lowest == std::numeric_limits<int64_t>::max() ? int64_t{0} : lowest;
        }
        if (costs.size() <= keep) {
            int64_t total = 0;
            for (const int64_t cost : costs) {
                total += cost;
            }
            return total;
        }

        // Keep the `keep` cheapest costs in a max heap.
        std::priority_queue<int64_t> cheapest;
        for (const int64_t cost : costs) {
            cheapest.push(cost);
            if (cheapest.size() > keep) {
                cheapest.pop();
            }
        }
        int64_t total = 0;
        while (!cheapest.empty()) {
            total += cheapest.top();
            cheapest.pop();
        }
        return total;
    }
};

} // namespace lucene
