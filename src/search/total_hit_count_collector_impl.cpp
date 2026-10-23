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

module lucene_core:total_hit_count_collector.impl;

import :total_hit_count_collector;

import std;

import :collector;
import :doc_id_set_iterator;
import :error;
import :index_searcher;
import :leaf_collector;
import :leaf_reader_context;
import :scorable;
import :score_mode;
import :types;
import :weight;

namespace lucene {

namespace {

/// Adds one hit per collected document.
class TotalHitCountLeafCollector final : public LeafCollector {
public:
    explicit TotalHitCountLeafCollector(TotalHitCountCollector &collector) : collector_(&collector) {}

    [[nodiscard]] VoidResult collect(int32_t /*doc*/, Scorable & /*scorer*/) override {
        collector_->add_hits(1);
        return {};
    }

    [[nodiscard]] std::string to_string() const override { return "TotalHitCountLeafCollector"; }

private:
    TotalHitCountCollector *collector_{nullptr};
};

} // namespace

Result<std::unique_ptr<LeafCollector>> TotalHitCountCollector::get_leaf_collector(const LeafReaderContext &context,
                                                                                 const Weight *weight,
                                                                                 const IndexSearcher &searcher) {
    if (weight != nullptr) {
        // Counting only reads the weight's state; the collector interface gets a
        // const pointer because collecting must not modify the weight.
        auto counted = const_cast<Weight *>(weight)->count(context, searcher);
        if (!counted.has_value()) {
            return std::unexpected(std::move(counted.error()));
        }
        if (counted.value() >= 0) {
            // The weight knows how many documents match without visiting them.
            add_hits(counted.value());
            return err(LuceneError::collection_terminated("leaf counted without collecting"));
        }
    }
    std::unique_ptr<LeafCollector> leaf(new TotalHitCountLeafCollector(*this));
    return leaf;
}

} // namespace lucene
