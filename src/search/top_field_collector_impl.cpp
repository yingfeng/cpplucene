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

module lucene_core:top_field_collector.impl;

import :top_field_collector;

import std;

import :collector;
import :error;
import :explanation;
import :field_comparator;
import :field_value_hit_queue;
import :index_searcher;
import :leaf_collector;
import :leaf_reader_context;
import :query;
import :scorable;
import :score_mode;
import :sort_field;
import :top_field_docs;
import :total_hits;
import :types;
import :weight;

namespace lucene {

/// The leaf collector of a [`TopFieldCollector`]: it keeps one leaf comparator
/// per sort field, copies the sort values of the competitive hits and lets the
/// queue drop the least competitive one.
class TopFieldCollector::Leaf final : public LeafCollector {
public:
    Leaf(TopFieldCollector &parent, int32_t doc_base, std::vector<std::unique_ptr<LeafFieldComparator>> comparators)
            : parent_(parent), doc_base_(doc_base), comparators_(std::move(comparators)) {}

    VoidResult set_scorer(Scorable &scorer) override {
        for (std::unique_ptr<LeafFieldComparator> &comparator : comparators_) {
            comparator->set_scorer(scorer);
        }
        return {};
    }

    VoidResult collect(int32_t doc, Scorable &scorer) override {
        ++parent_.total_hits_;
        FieldValueHitQueue &queue = parent_.queue();
        const int32_t global_doc = doc + doc_base_;

        float score = 0.0F;
        if (parent_.needs_scores_) {
            auto current = scorer.score();
            if (!current.has_value()) {
                return std::unexpected(std::move(current.error()));
            }
            score = current.value();
        }

        if (!queue.is_full()) {
            const int32_t slot = static_cast<int32_t>(queue.size());
            for (std::unique_ptr<LeafFieldComparator> &comparator : comparators_) {
                if (auto copied = comparator->copy(slot, doc); !copied.has_value()) {
                    return std::unexpected(std::move(copied.error()));
                }
            }
            queue.push(FieldValueEntry{slot, global_doc, score});
            return update_bottom();
        }

        // The queue is full: the hit has to sort before the least competitive
        // one, which is decided by the first field that is not a tie.
        bool competitive = false;
        for (std::unique_ptr<LeafFieldComparator> &comparator : comparators_) {
            auto compared = comparator->compare_bottom(doc);
            if (!compared.has_value()) {
                return std::unexpected(std::move(compared.error()));
            }
            if (compared.value() < 0) {
                competitive = true;
                break;
            }
            if (compared.value() > 0) {
                return {};
            }
        }
        if (!competitive) {
            // Equal sort values: the document is the larger one, so it loses.
            return {};
        }

        const int32_t slot = queue.top().slot;
        for (std::unique_ptr<LeafFieldComparator> &comparator : comparators_) {
            if (auto copied = comparator->copy(slot, doc); !copied.has_value()) {
                return std::unexpected(std::move(copied.error()));
            }
        }
        queue.update_top(global_doc, score);
        return update_bottom();
    }

    [[nodiscard]] std::string to_string() const override { return "TopFieldCollector.Leaf"; }

private:
    /// Tells both the leaf comparators and the queue comparators where the new
    /// bottom is.
    [[nodiscard]] VoidResult update_bottom() {
        FieldValueHitQueue &queue = parent_.queue();
        const int32_t slot = queue.top().slot;
        auto &queue_comparators = queue.comparators();
        for (size_t i = 0; i < comparators_.size(); ++i) {
            comparators_[i]->set_bottom(slot);
            queue_comparators[i]->set_bottom(slot);
        }
        return {};
    }

    TopFieldCollector &parent_;
    int32_t doc_base_{0};
    std::vector<std::unique_ptr<LeafFieldComparator>> comparators_;
};

TopFieldCollector::TopFieldCollector(Sort sort, std::unique_ptr<FieldValueHitQueue> queue)
        : sort_(std::move(sort)), queue_(std::move(queue)), needs_scores_(sort_.needs_scores()) {}

Result<std::unique_ptr<TopFieldCollector>> TopFieldCollector::create(const Sort &sort, int32_t num_hits) {
    if (num_hits < 0) {
        return err(LuceneError::illegal_argument("numHits must be >= 0, got " + std::to_string(num_hits)));
    }
    auto queue = FieldValueHitQueue::create(sort.fields(), num_hits);
    if (!queue.has_value()) {
        return std::unexpected(std::move(queue.error()));
    }
    return std::unique_ptr<TopFieldCollector>(
            new TopFieldCollector(sort, std::make_unique<FieldValueHitQueue>(std::move(queue.value()))));
}

Result<std::unique_ptr<LeafCollector>> TopFieldCollector::get_leaf_collector(const LeafReaderContext &context,
                                                                            const Weight * /*weight*/,
                                                                            const IndexSearcher & /*searcher*/) {
    std::vector<std::unique_ptr<LeafFieldComparator>> comparators;
    comparators.reserve(queue_->comparators().size());
    for (std::unique_ptr<FieldComparator> &comparator : queue_->comparators()) {
        auto leaf = comparator->get_leaf_comparator(context);
        if (!leaf.has_value()) {
            return std::unexpected(std::move(leaf.error()));
        }
        std::unique_ptr<LeafFieldComparator> &leaf_comparator = leaf.value();
        if (queue_->size() > 0) {
            // Seed the leaf comparator with the bottom of the hits collected so
            // far, so that it can prune the first documents of this leaf.
            leaf_comparator->set_bottom(queue_->top().slot);
        }
        comparators.push_back(std::move(leaf_comparator));
    }
    std::unique_ptr<LeafCollector> collector(
            new Leaf(*this, context.doc_base(), std::move(comparators)));
    return collector;
}

Result<TopFieldDocs> TopFieldCollector::top_docs(int32_t start) {
    std::vector<FieldValueEntry> entries = queue_->take_all(); // least competitive first
    std::vector<FieldDoc> hits;
    const int32_t available = static_cast<int32_t>(entries.size());
    const int32_t wanted = start < 0 ? 0 : available - start;
    hits.reserve(static_cast<size_t>(std::max(wanted, 0)));

    // The queue pops the least competitive hit first, so the best hit is the
    // last one, and skipping `start` hits means starting that much further down.
    for (int32_t i = available - 1 - start; i >= 0; --i) {
        const FieldValueEntry &entry = entries[static_cast<size_t>(i)];
        std::vector<SortValue> values;
        values.reserve(queue_->comparators().size());
        for (std::unique_ptr<FieldComparator> &comparator : queue_->comparators()) {
            auto value = comparator->value(entry.slot);
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            values.push_back(std::move(value.value()));
        }
        hits.push_back(FieldDoc::create(entry.doc, entry.score, std::move(values)));
    }

    const TotalHits::Relation relation =
            needs_scores_ ? TotalHits::Relation::GreaterThanOrEqualTo : TotalHits::Relation::EqualTo;
    return TopFieldDocs(TotalHits::create(static_cast<size_t>(total_hits_), relation), std::move(hits), sort_);
}

} // namespace lucene
