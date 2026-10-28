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

module lucene_core:top_field_collector_manager.impl;

import :top_field_collector_manager;

import std;

import :collector;
import :error;
import :max_score_accumulator;
import :sort_field;
import :top_docs;
import :top_field_collector;
import :top_field_docs;
import :types;

namespace lucene {

TopFieldCollectorManager::TopFieldCollectorManager(Sort sort, int32_t num_hits, std::optional<FieldDoc> after,
                                                   size_t total_hits_threshold)
        : sort_(std::move(sort)), num_hits_(num_hits), after_(std::move(after)),
          total_hits_threshold_(total_hits_threshold), min_score_acc_(std::make_shared<MaxScoreAccumulator>()) {}

Result<std::unique_ptr<TopFieldCollectorManager>> TopFieldCollectorManager::create(const Sort &sort,
                                                                                  int32_t num_hits,
                                                                                  size_t total_hits_threshold) {
    if (num_hits < 0) {
        return err(LuceneError::illegal_argument("numHits must be >= 0, got " + std::to_string(num_hits)));
    }
    return std::unique_ptr<TopFieldCollectorManager>(
            new TopFieldCollectorManager(sort, num_hits, std::nullopt, total_hits_threshold));
}

Result<std::unique_ptr<TopFieldCollectorManager>> TopFieldCollectorManager::with_after(const Sort &sort,
                                                                                       int32_t num_hits,
                                                                                       const FieldDoc &after,
                                                                                       size_t total_hits_threshold) {
    if (num_hits < 0) {
        return err(LuceneError::illegal_argument("numHits must be >= 0, got " + std::to_string(num_hits)));
    }
    if (after.fields.empty()) {
        return err(LuceneError::illegal_argument(
                "`after.fields` wasn't set; the previous search must fill in the sort values of its last hit"));
    }
    if (after.fields.size() != sort.fields().size()) {
        return err(LuceneError::illegal_argument("`after.fields` has " + std::to_string(after.fields.size()) +
                                                 " values but the sort has " + std::to_string(sort.fields().size())));
    }
    return std::unique_ptr<TopFieldCollectorManager>(
            new TopFieldCollectorManager(sort, num_hits, std::optional<FieldDoc>(after), total_hits_threshold));
}

Result<std::unique_ptr<Collector>> TopFieldCollectorManager::new_collector() const {
    auto collector = TopFieldCollector::create(sort_, num_hits_, after_.has_value() ? &after_.value() : nullptr,
                                               total_hits_threshold_, min_score_acc_);
    if (!collector.has_value()) {
        return std::unexpected(std::move(collector.error()));
    }
    std::unique_ptr<Collector> result = std::move(collector.value());
    return result;
}

Result<std::vector<TopDocs>>
TopFieldCollectorManager::reduce(std::vector<std::unique_ptr<Collector>> collectors) const {
    auto merged = reduce_field_docs(std::move(collectors));
    if (!merged.has_value()) {
        return std::unexpected(std::move(merged.error()));
    }
    std::vector<TopDocs> results;
    results.push_back(TopDocs::create(merged.value().total_hits, merged.value().as_score_docs()));
    return results;
}

Result<TopFieldDocs>
TopFieldCollectorManager::reduce_field_docs(std::vector<std::unique_ptr<Collector>> collectors) const {
    if (collectors.size() == 1) {
        // A single collector already has the final order and its sort values.
        auto *collector = dynamic_cast<TopFieldCollector *>(collectors.front().get());
        if (collector == nullptr) {
            return err(LuceneError::illegal_state("a TopFieldCollectorManager was handed a foreign collector"));
        }
        return collector->top_docs();
    }

    std::vector<TopFieldDocs> shards;
    shards.reserve(collectors.size());
    for (std::unique_ptr<Collector> &collector : collectors) {
        auto *field_collector = dynamic_cast<TopFieldCollector *>(collector.get());
        if (field_collector == nullptr) {
            return err(LuceneError::illegal_state("a TopFieldCollectorManager was handed a foreign collector"));
        }
        auto top = field_collector->top_docs();
        if (!top.has_value()) {
            return std::unexpected(std::move(top.error()));
        }
        shards.push_back(std::move(top.value()));
    }
    // Every shard is sorted by the same sort, so the merge only has to compare
    // the sort values of the shard heads.
    return merge_top_field_docs_with_start(sort_, 0, static_cast<size_t>(num_hits_), shards);
}

} // namespace lucene
