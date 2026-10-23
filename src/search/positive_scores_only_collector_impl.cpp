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

module lucene_core:positive_scores_only_collector.impl;

import :positive_scores_only_collector;

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

/// Forwards the hits whose score is strictly positive.
class PositiveScoresOnlyLeafCollector final : public LeafCollector {
public:
    explicit PositiveScoresOnlyLeafCollector(std::unique_ptr<LeafCollector> inner) : inner_(std::move(inner)) {}

    [[nodiscard]] VoidResult set_scorer(Scorable &scorer) override { return inner_->set_scorer(scorer); }

    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &scorer) override {
        auto score = scorer.score();
        if (!score.has_value()) {
            return std::unexpected(std::move(score.error()));
        }
        if (score.value() > 0.0F) {
            return inner_->collect(doc, scorer);
        }
        return {};
    }

    [[nodiscard]] Result<std::unique_ptr<DocIdSetIterator>> competitive_iterator() override {
        return inner_->competitive_iterator();
    }

    [[nodiscard]] VoidResult finish() override { return inner_->finish(); }

    [[nodiscard]] std::string to_string() const override {
        return "PositiveScoresOnlyLeafCollector(" + inner_->to_string() + ")";
    }

private:
    std::unique_ptr<LeafCollector> inner_;
};

} // namespace

Result<std::unique_ptr<LeafCollector>> PositiveScoresOnlyCollector::get_leaf_collector(const LeafReaderContext &context,
                                                                                      const Weight *weight,
                                                                                      const IndexSearcher &searcher) {
    auto inner = inner_->get_leaf_collector(context, weight, searcher);
    if (!inner.has_value()) {
        return std::unexpected(std::move(inner.error()));
    }
    std::unique_ptr<LeafCollector> leaf(
            new PositiveScoresOnlyLeafCollector(std::move(inner.value())));
    return leaf;
}

} // namespace lucene
