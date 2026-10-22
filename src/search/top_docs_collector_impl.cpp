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

module lucene_core:top_docs_collector.impl;

import :top_docs_collector;

import :error;
import :hit_queue;
import :score_doc;
import :top_docs;
import :total_hits;
import :types;

import std;

namespace lucene {

TopDocsCollectorBase::TopDocsCollectorBase(HitQueue queue) : pq(std::move(queue)) {}

TopDocs TopDocsCollectorBase::empty_top_docs() {
    return TopDocs::create(TotalHits::create(0, TotalHits::Relation::EqualTo), {});
}

size_t TopDocsCollectorBase::top_docs_size() const { return std::min(total_hits, pq.size()); }

Result<TopDocs> TopDocsCollectorBase::top_docs() {
    const auto size = static_cast<int32_t>(top_docs_size());
    return top_docs_with_start_limit(0, size);
}

Result<TopDocs> TopDocsCollectorBase::top_docs_with_start(int32_t start) {
    // When the queue was pre-populated with sentinels there may be fewer real
    // results than its size, so clamp to the hit count.
    const auto size = static_cast<int32_t>(top_docs_size());
    return top_docs_with_start_limit(start, size);
}

Result<TopDocs> TopDocsCollectorBase::top_docs_with_start_limit(int32_t start, int32_t how_many) {
    const auto size = static_cast<int32_t>(top_docs_size());

    if (how_many < 0) {
        return err(LuceneError::illegal_argument("Number of hits requested must be greater than 0 but value was " +
                                                 std::to_string(how_many)));
    }
    if (start < 0) {
        return err(LuceneError::illegal_argument("Expected value of starting position is between 0 and " +
                                                 std::to_string(size) + ", got " + std::to_string(start)));
    }
    if (start >= size || how_many == 0) {
        return new_top_docs(std::nullopt, start);
    }

    const int32_t wanted = std::min(size - start, how_many);
    std::vector<ScoreDoc> results(static_cast<size_t>(wanted));

    // Drop the hits worse than the requested window, then the ones inside it
    // that come before `start`.
    const auto discard_count = static_cast<int32_t>(pq.size()) - start - wanted;
    for (int32_t i = 0; i < discard_count; ++i) {
        if (auto popped = pq.pop_unchecked(); !popped.has_value()) {
            return std::unexpected(std::move(popped.error()));
        }
    }

    if (auto populated = populate_results(results, static_cast<size_t>(wanted)); !populated.has_value()) {
        return std::unexpected(std::move(populated.error()));
    }
    return new_top_docs(std::move(results), start);
}

TopDocs TopDocsCollectorBase::new_top_docs(std::optional<std::vector<ScoreDoc>> results, int32_t /*start*/) const {
    // Unlike rlucene's trait default, the hit count survives an empty result
    // page: that is what `TopScoreDocCollector` does, and it is the only
    // `TopDocs` collector in the port.
    if (!results.has_value()) {
        return TopDocs::create(TotalHits::create(total_hits, total_hits_relation), {});
    }
    return TopDocs::create(TotalHits::create(total_hits, total_hits_relation), std::move(results.value()));
}

VoidResult TopDocsCollectorBase::populate_results(std::span<ScoreDoc> results, size_t how_many) {
    // The queue pops worst first, so fill the array backwards.
    for (size_t i = how_many; i > 0; --i) {
        auto popped = pq.pop_unchecked();
        if (!popped.has_value()) {
            return std::unexpected(std::move(popped.error()));
        }
        results[i - 1] = popped.value();
    }
    return {};
}

} // namespace lucene
