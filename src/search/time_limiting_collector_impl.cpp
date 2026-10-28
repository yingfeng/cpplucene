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

module lucene_core:time_limiting_collector.impl;

import :time_limiting_collector;

import :collector;
import :collector_manager;
import :error;
import :leaf_collector;
import :query_timeout;
import :score_mode;
import :scorable;
import :top_docs;
import :types;
import :weight;

import std;

namespace lucene {

export class IndexSearcher;

namespace {

/// Counts the documents of one leaf and stops when the timeout fired.
class TimeLimitingLeafCollector final : public LeafCollector {
public:
    TimeLimitingLeafCollector(std::unique_ptr<LeafCollector> inner, TimeLimitingCollector &parent)
        : inner_(std::move(inner)), parent_(&parent) {}

    [[nodiscard]] VoidResult set_scorer(Scorable &scorer) override { return inner_->set_scorer(scorer); }

    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &scorer) override {
        // The first document of a leaf checks too, like the reference
        // implementation's `counter & ticks_mask` test.
        if (collected_ % parent_->ticks() == 0 && parent_->timeout_expired()) {
            return err(LuceneError::time_exceeded("the search timed out after " + std::to_string(collected_) +
                                                 " documents"));
        }
        ++collected_;
        return inner_->collect(doc, scorer);
    }

    [[nodiscard]] Result<std::unique_ptr<DocIdSetIterator>> competitive_iterator() override {
        return inner_->competitive_iterator();
    }

    [[nodiscard]] VoidResult finish() override { return inner_->finish(); }

    [[nodiscard]] std::string to_string() const override { return inner_->to_string(); }

private:
    std::unique_ptr<LeafCollector> inner_;
    TimeLimitingCollector *parent_;
    int64_t collected_{0};
};

} // namespace

// ---- TimeLimitingCollector ----

TimeLimitingCollector::TimeLimitingCollector(std::unique_ptr<Collector> collector, const QueryTimeout &timeout,
                                             int32_t ticks)
        : collector_(std::move(collector)), timeout_(&timeout), ticks_(ticks < 1 ? 1 : ticks) {}

Result<std::unique_ptr<LeafCollector>> TimeLimitingCollector::get_leaf_collector(const LeafReaderContext &context,
                                                                                const Weight *weight,
                                                                                const IndexSearcher &searcher) {
    auto leaf = collector_->get_leaf_collector(context, weight, searcher);
    if (!leaf.has_value()) {
        return std::unexpected(std::move(leaf.error()));
    }
    return std::unique_ptr<LeafCollector>(new TimeLimitingLeafCollector(std::move(leaf.value()), *this));
}

ScoreMode TimeLimitingCollector::score_mode() const {
    return collector_->score_mode();
}

VoidResult TimeLimitingCollector::set_weight(const Weight *weight) {
    return collector_->set_weight(weight);
}

std::unique_ptr<Collector> TimeLimitingCollector::take_collector() {
    return std::move(collector_);
}

// ---- TimeLimitingCollectorManager ----

TimeLimitingCollectorManager::TimeLimitingCollectorManager(const CollectorManager &inner,
                                                           const QueryTimeout &timeout, int32_t ticks)
        : inner_(&inner), timeout_(&timeout), ticks_(ticks < 1 ? 1 : ticks) {}

Result<std::unique_ptr<Collector>> TimeLimitingCollectorManager::new_collector() const {
    auto inner = inner_->new_collector();
    if (!inner.has_value()) {
        return std::unexpected(std::move(inner.error()));
    }
    return std::unique_ptr<Collector>(
            new TimeLimitingCollector(std::move(inner.value()), *timeout_, ticks_));
}

Result<std::vector<TopDocs>>
TimeLimitingCollectorManager::reduce(std::vector<std::unique_ptr<Collector>> collectors) const {
    std::vector<std::unique_ptr<Collector>> unwrapped;
    unwrapped.reserve(collectors.size());
    for (std::unique_ptr<Collector> &collector : collectors) {
        auto *wrapping = dynamic_cast<TimeLimitingCollector *>(collector.get());
        if (wrapping == nullptr) {
            return err(LuceneError::illegal_argument(
                    "the collectors given to a TimeLimitingCollectorManager have to come from it"));
        }
        unwrapped.push_back(wrapping->take_collector());
    }
    return inner_->reduce(std::move(unwrapped));
}

} // namespace lucene
