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

module lucene_core:disjunction_max_bulk_scorer.impl;

import :disjunction_max_bulk_scorer;

import std;

import :bits;
import :bulk_scorer;
import :debug;
import :doc_id_set_iterator;
import :error;
import :fixed_bit_set;
import :leaf_collector;
import :priority_queue;
import :scorable;
import :simple_scorable;
import :types;

namespace lucene {

namespace {

/// Records the documents of one window and the best score each of them got.
class WindowCollector final : public LeafCollector {
public:
    WindowCollector(FixedBitSet &window_matches, SimpleScorable &top_level_scorable, int32_t window_min,
                    std::vector<float> &window_scores)
            : window_matches_(window_matches),
              top_level_scorable_(top_level_scorable),
              window_min_(window_min),
              window_scores_(window_scores) {}

    [[nodiscard]] VoidResult set_scorer(Scorable &scorer) override {
        if (top_level_scorable_.min_competitive_score() != 0.0F) {
            return scorer.set_min_competitive_score(top_level_scorable_.min_competitive_score());
        }
        return {};
    }

    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &scorer) override {
        const size_t delta = static_cast<size_t>(doc - window_min_);
        if (delta >= window_scores_.size()) {
            return err(LuceneError::array_index_out_of_bounds("the document is outside the window"));
        }
        window_matches_.set(delta);
        auto score = scorer.score();
        if (!score.has_value()) {
            return std::unexpected(std::move(score.error()));
        }
        // Several sub-scorers matching the same document keep the best score.
        if (score.value() > window_scores_[delta]) {
            window_scores_[delta] = score.value();
        }
        return {};
    }

private:
    FixedBitSet &window_matches_;
    SimpleScorable &top_level_scorable_;
    int32_t window_min_;
    std::vector<float> &window_scores_;
};

} // namespace

DisjunctionMaxBulkScorer::DisjunctionMaxBulkScorer(std::shared_ptr<std::vector<Entry>> entries,
                                                   PriorityQueue<size_t, EntryCompare> queue)
        : entries_(std::move(entries)),
          window_matches_(kDisjunctionMaxWindowSize + 1),
          window_scores_(kDisjunctionMaxWindowSize, 0.0F),
          queue_(std::move(queue)),
          top_level_scorable_() {}

Result<std::unique_ptr<DisjunctionMaxBulkScorer>>
DisjunctionMaxBulkScorer::create(std::vector<std::unique_ptr<BulkScorer>> scorers) {
    if (scorers.size() < 2) {
        return err(LuceneError::illegal_argument("a disjunction-max bulk scorer needs at least 2 scorers"));
    }
    auto entries = std::make_shared<std::vector<Entry>>();
    entries->reserve(scorers.size());
    for (auto &scorer : scorers) {
        entries->push_back(Entry{std::move(scorer), 0});
    }

    auto queue = PriorityQueue<size_t, EntryCompare>::create(entries->size(), EntryCompare{entries});
    if (!queue.has_value()) {
        return std::unexpected(std::move(queue.error()));
    }
    auto built = std::move(queue.value());
    for (size_t index = 0; index < entries->size(); ++index) {
        auto added = built.add(index);
        if (!added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
    }

    std::unique_ptr<DisjunctionMaxBulkScorer> scorer(new DisjunctionMaxBulkScorer(std::move(entries), std::move(built)));
    return scorer;
}

Result<int32_t> DisjunctionMaxBulkScorer::score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                                int32_t max) {
    for (;;) {
        int32_t window_min = 0;
        int32_t window_max = 0;
        {
            const size_t *top = queue_.top();
            if (top == nullptr) {
                return err(LuceneError::illegal_state("the queue is empty"));
            }
            const int32_t top_next = (*entries_)[*top].next;
            if (top_next >= max) {
                break;
            }
            window_min = std::max(top_next, min);
            window_max = static_cast<int32_t>(
                    std::min<int64_t>(max, static_cast<int64_t>(window_min) + static_cast<int64_t>(kDisjunctionMaxWindowSize)));
        }

        // Every sub-scorer fills the window; a sub-scorer stops contributing once
        // its next document is past the window.
        for (;;) {
            const size_t *top = queue_.top();
            if (top == nullptr) {
                return err(LuceneError::illegal_state("the queue is empty"));
            }
            const size_t index = *top;
            Entry &entry = (*entries_)[index];
            WindowCollector window_collector(window_matches_, top_level_scorable_, window_min, window_scores_);
            auto next = entry.scorer->score(window_collector, accept_docs, window_min, window_max);
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            entry.next = next.value();

            auto updated = queue_.update_top();
            if (!updated.has_value()) {
                return std::unexpected(std::move(updated.error()));
            }
            size_t *new_top = updated.value();
            if (new_top == nullptr) {
                return err(LuceneError::illegal_state("the queue is empty"));
            }
            if ((*entries_)[*new_top].next >= window_max) {
                break;
            }
        }

        auto set = collector.set_scorer(top_level_scorable_);
        if (!set.has_value()) {
            return std::unexpected(std::move(set.error()));
        }

        int64_t window_doc = window_matches_.next_set_bit(0);
        while (window_doc >= 0) {
            const int32_t doc = window_min + static_cast<int32_t>(window_doc);
            const float window_score = window_scores_[static_cast<size_t>(window_doc)];
            auto scored = top_level_scorable_.set_score(window_score);
            if (!scored.has_value()) {
                return std::unexpected(std::move(scored.error()));
            }
            auto collected = collector.collect(doc, top_level_scorable_);
            if (!collected.has_value()) {
                return std::unexpected(std::move(collected.error()));
            }
            window_doc = window_matches_.next_set_bit(static_cast<size_t>(window_doc) + 1);
        }

        window_matches_.clear_all();
        std::ranges::fill(window_scores_, 0.0F);
    }

    const size_t *top = queue_.top();
    if (top == nullptr) {
        return err(LuceneError::illegal_state("the queue is empty"));
    }
    return (*entries_)[*top].next;
}

Result<int64_t> DisjunctionMaxBulkScorer::cost() {
    int64_t cost = 0;
    for (auto &entry : *entries_) {
        auto sub_cost = entry.scorer->cost();
        if (!sub_cost.has_value()) {
            return std::unexpected(std::move(sub_cost.error()));
        }
        cost += sub_cost.value();
    }
    return cost;
}

} // namespace lucene
