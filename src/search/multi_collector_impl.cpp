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

module lucene_core:multi_collector.impl;

import :multi_collector;

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

/// Fans every hit out to the leaf collectors of the wrapped collectors.
class MultiLeafCollector final : public LeafCollector {
public:
    explicit MultiLeafCollector(std::vector<std::unique_ptr<LeafCollector>> collectors)
            : collectors_(std::move(collectors)) {}

    [[nodiscard]] VoidResult set_scorer(Scorable &scorer) override {
        for (std::unique_ptr<LeafCollector> &collector : collectors_) {
            if (auto set = collector->set_scorer(scorer); !set.has_value()) {
                return std::unexpected(std::move(set.error()));
            }
        }
        return {};
    }

    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &scorer) override {
        for (std::unique_ptr<LeafCollector> &collector : collectors_) {
            if (auto collected = collector->collect(doc, scorer); !collected.has_value()) {
                return std::unexpected(std::move(collected.error()));
            }
        }
        return {};
    }

    [[nodiscard]] VoidResult finish() override {
        for (std::unique_ptr<LeafCollector> &collector : collectors_) {
            if (auto finished = collector->finish(); !finished.has_value()) {
                return std::unexpected(std::move(finished.error()));
            }
        }
        return {};
    }

    [[nodiscard]] std::string to_string() const override { return "MultiLeafCollector"; }

private:
    std::vector<std::unique_ptr<LeafCollector>> collectors_;
};

} // namespace

MultiCollector::MultiCollector(std::vector<Collector *> collectors) : collectors_(std::move(collectors)) {
    // The combined mode has to satisfy every wrapped collector: scores are
    // collected as soon as one of them needs them, and the search has to be
    // exhaustive as soon as one of them cannot skip documents.
    bool needs_scores = false;
    bool exhaustive = true;
    for (const Collector *collector : collectors_) {
        const ScoreMode mode = collector->score_mode();
        needs_scores = needs_scores || score_mode_needs_scores(mode);
        exhaustive = exhaustive && score_mode_is_exhaustive(mode);
    }
    if (needs_scores) {
        score_mode_ = exhaustive ? ScoreMode::Complete : ScoreMode::TopScores;
    } else {
        score_mode_ = exhaustive ? ScoreMode::CompleteNoScores : ScoreMode::TopDocs;
    }
}

Result<std::unique_ptr<MultiCollector>> MultiCollector::wrap(std::vector<Collector *> collectors) {
    if (collectors.empty()) {
        return err(LuceneError::illegal_argument("MultiCollector requires at least one collector"));
    }
    return std::unique_ptr<MultiCollector>(new MultiCollector(std::move(collectors)));
}

Result<std::unique_ptr<LeafCollector>> MultiCollector::get_leaf_collector(const LeafReaderContext &context,
                                                                         const Weight *weight,
                                                                         const IndexSearcher &searcher) {
    std::vector<std::unique_ptr<LeafCollector>> leaves;
    leaves.reserve(collectors_.size());
    for (Collector *collector : collectors_) {
        auto leaf = collector->get_leaf_collector(context, weight, searcher);
        if (!leaf.has_value()) {
            return std::unexpected(std::move(leaf.error()));
        }
        leaves.push_back(std::move(leaf.value()));
    }
    std::unique_ptr<LeafCollector> leaf(new MultiLeafCollector(std::move(leaves)));
    return leaf;
}

VoidResult MultiCollector::set_weight(const Weight *weight) {
    for (Collector *collector : collectors_) {
        if (auto set = collector->set_weight(weight); !set.has_value()) {
            return std::unexpected(std::move(set.error()));
        }
    }
    return {};
}

} // namespace lucene
