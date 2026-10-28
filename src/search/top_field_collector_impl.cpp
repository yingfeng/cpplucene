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
import :max_score_accumulator;
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
    Leaf(TopFieldCollector &parent, int32_t doc_base, std::vector<std::unique_ptr<LeafFieldComparator>> comparators,
         bool early_termination_possible)
            : parent_(parent), doc_base_(doc_base), comparators_(std::move(comparators)),
              after_doc_(parent.after_doc_ - doc_base), early_termination_possible_(early_termination_possible) {}

    VoidResult set_scorer(Scorable &scorer) override {
        for (std::unique_ptr<LeafFieldComparator> &comparator : comparators_) {
            comparator->set_scorer(scorer);
        }
        return {};
    }

    VoidResult collect(int32_t doc, Scorable &scorer) override {
        // Every match counts, including the ones a paging cursor skips, which is
        // what the reference implementation does.
        ++parent_.total_hits_;
        if (parent_.total_hits_relation_ == TotalHits::Relation::EqualTo &&
            parent_.total_hits_ > parent_.total_hits_threshold_) {
            // From here on the reported total is only a lower bound, and a
            // comparator that competes on a value which can no longer win (a
            // score) is told to stop competing.
            parent_.total_hits_relation_ = TotalHits::Relation::GreaterThanOrEqualTo;
            if (!comparators_.empty()) {
                comparators_.front()->set_hits_threshold_reached();
            }
        }

        FieldValueHitQueue &queue = parent_.queue();

        // A full queue decides whether the hit can still make it; a not yet full
        // queue takes it (the cursor below may still drop it).
        if (queue.is_full()) {
            bool competitive = false;
            if (!collected_all_competitive_hits_) {
                // The hit has to sort before the least competitive one, which is
                // decided by the first field that is not a tie.
                for (std::unique_ptr<LeafFieldComparator> &comparator : comparators_) {
                    auto compared = comparator->compare_bottom(doc);
                    if (!compared.has_value()) {
                        return std::unexpected(std::move(compared.error()));
                    }
                    if (compared.value() != 0) {
                        // Negative means the document sorts before the bottom.
                        competitive = compared.value() < 0;
                        break;
                    }
                }
            }
            if (!competitive) {
                if (early_termination_possible_) {
                    // The documents of an index sorted leaf arrive in sort order,
                    // so once one of them is not competitive no later one is:
                    // stop, unless the hit count still has to be exact.
                    if (parent_.total_hits_ > parent_.total_hits_threshold_) {
                        parent_.total_hits_relation_ = TotalHits::Relation::GreaterThanOrEqualTo;
                        return err(LuceneError::collection_terminated(
                                "collection terminated due to early termination threshold"));
                    }
                    collected_all_competitive_hits_ = true;
                }
                return {};
            }
        }

        if (parent_.paging_) {
            // The hits of the previous pages are found again by this search:
            // those that sort before the cursor, and those that tie it but come
            // before its document (documents are visited in increasing order).
            int32_t compared_top = 0;
            for (std::unique_ptr<LeafFieldComparator> &comparator : comparators_) {
                auto compared = comparator->compare_top(doc);
                if (!compared.has_value()) {
                    return std::unexpected(std::move(compared.error()));
                }
                if (compared.value() != 0) {
                    compared_top = compared.value();
                    break;
                }
            }
            if (compared_top < 0 || (compared_top == 0 && doc <= after_doc_)) {
                return {};
            }
        }

        const int32_t global_doc = doc + doc_base_;

        float score = 0.0F;
        if (parent_.needs_scores_) {
            auto current = scorer.score();
            if (!current.has_value()) {
                return std::unexpected(std::move(current.error()));
            }
            score = current.value();
        }

        if (parent_.min_score_acc_ != nullptr &&
            (parent_.total_hits_ & static_cast<int64_t>(parent_.min_score_acc_->mod_interval() - 1)) == 0) {
            if (auto updated = update_global_min_competitive_score(scorer); !updated.has_value()) {
                return updated;
            }
        }

        if (!queue.is_full()) {
            const int32_t slot = static_cast<int32_t>(queue.size());
            for (std::unique_ptr<LeafFieldComparator> &comparator : comparators_) {
                if (auto copied = comparator->copy(slot, doc); !copied.has_value()) {
                    return std::unexpected(std::move(copied.error()));
                }
            }
            queue.push(FieldValueEntry{slot, global_doc, score});
            if (auto updated = update_bottom(); !updated.has_value()) {
                return updated;
            }
            return update_min_competitive_score(scorer);
        }

        // Competitive, so it replaces the least competitive hit of the queue.
        const int32_t slot = queue.top().slot;
        for (std::unique_ptr<LeafFieldComparator> &comparator : comparators_) {
            if (auto copied = comparator->copy(slot, doc); !copied.has_value()) {
                return std::unexpected(std::move(copied.error()));
            }
        }
        queue.update_top(global_doc, score);
        if (auto updated = update_bottom(); !updated.has_value()) {
            return updated;
        }
        return update_min_competitive_score(scorer);
    }

    /// Tells the scorer that a hit below `min_competitive_score_` cannot make the
    /// queue any more, which lets a scorer that knows about impacts skip it.
    ///
    /// Port of `update_min_competitive_score` (`core/search/top_field_collector.rs`).
    [[nodiscard]] VoidResult update_min_competitive_score(Scorable &scorer) {
        TopFieldCollector &parent = parent_;
        FieldValueHitQueue &queue = parent.queue();
        if (!parent.can_set_min_score_ || !queue.is_full() || parent.total_hits_ <= parent.total_hits_threshold_) {
            return {};
        }
        // The queue is a min-heap on the sort values, so the first comparator's value
        // of the top entry is the score of the least competitive hit.
        const float min_score = queue.top().score;
        if (min_score > parent.min_competitive_score_) {
            if (auto set = scorer.set_min_competitive_score(min_score); !set.has_value()) {
                return set;
            }
            parent.min_competitive_score_ = min_score;
            parent.total_hits_relation_ = TotalHits::Relation::GreaterThanOrEqualTo;
            if (parent.min_score_acc_ != nullptr) {
                parent.min_score_acc_->accumulate(doc_base_, min_score);
            }
        }
        return {};
    }

    /// Picks up the score another collector of the same search published.
    ///
    /// Port of `update_global_min_competitive_score`
    /// (`core/search/top_field_collector.rs`); the accumulator is only read every
    /// few documents, which is what its interval is for.
    [[nodiscard]] VoidResult update_global_min_competitive_score(Scorable &scorer) {
        TopFieldCollector &parent = parent_;
        if (parent.min_score_acc_ == nullptr || !parent.can_set_min_score_) {
            return {};
        }
        const int64_t raw = parent.min_score_acc_->get_raw();
        if (raw == std::numeric_limits<int64_t>::min()) {
            return {};
        }
        const float score = MaxScoreAccumulator::to_score(raw);
        if (score > parent.min_competitive_score_) {
            if (auto set = scorer.set_min_competitive_score(score); !set.has_value()) {
                return set;
            }
            parent.min_competitive_score_ = score;
            parent.total_hits_relation_ = TotalHits::Relation::GreaterThanOrEqualTo;
        }
        return {};
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
    /// The document of the `after` cursor in this leaf's numbering.
    int32_t after_doc_{0};
    /// Whether this leaf is sorted like the search, which is what allows the
    /// collection to stop at the first non-competitive hit.
    bool early_termination_possible_{false};
    /// Set once the queue is full and a non-competitive hit was seen: every later
    /// hit of an index sorted leaf is non-competitive as well, so the comparisons
    /// can be skipped.
    bool collected_all_competitive_hits_{false};
};

TopFieldCollector::TopFieldCollector(Sort sort, std::unique_ptr<FieldValueHitQueue> queue)
        : sort_(std::move(sort)), queue_(std::move(queue)), needs_scores_(sort_.needs_scores()) {}

Result<std::unique_ptr<TopFieldCollector>> TopFieldCollector::create(const Sort &sort, int32_t num_hits,
                                                                    const FieldDoc *after,
                                                                    size_t total_hits_threshold,
                                                                    std::shared_ptr<MaxScoreAccumulator> min_score_acc) {
    if (num_hits < 0) {
        return err(LuceneError::illegal_argument("numHits must be >= 0, got " + std::to_string(num_hits)));
    }
    if (after != nullptr && after->fields.size() != sort.fields().size()) {
        return err(LuceneError::illegal_argument("the after cursor has " + std::to_string(after->fields.size()) +
                                                 " sort values, but the sort has " +
                                                 std::to_string(sort.fields().size())));
    }
    auto queue = FieldValueHitQueue::create(sort.fields(), num_hits);
    if (!queue.has_value()) {
        return std::unexpected(std::move(queue.error()));
    }
    std::unique_ptr<TopFieldCollector> collector(
            new TopFieldCollector(sort, std::make_unique<FieldValueHitQueue>(std::move(queue.value()))));
    // Counting fewer hits than the queue holds would report a total below the
    // number of hits returned, so the threshold never goes below `num_hits`.
    collector->total_hits_threshold_ = std::max(total_hits_threshold, static_cast<size_t>(num_hits));
    collector->min_score_acc_ = std::move(min_score_acc);
    // A hit may only be skipped on its score when the sort is by relevance in its
    // natural direction (best first) and the hit count is not exact anyway, which
    // is the same condition the reference implementation uses.
    collector->can_set_min_score_ =
            dynamic_cast<ScoreFieldComparator *>(collector->queue_->comparators().front().get()) != nullptr &&
            !sort.fields().front().reverse() &&
            collector->total_hits_threshold_ != std::numeric_limits<size_t>::max();
    if (after == nullptr) {
        return collector;
    }
    collector->paging_ = true;
    collector->after_doc_ = after->doc;
    // The comparators keep the top value, which is where the leaf comparators
    // read it from when they decide whether a document sorts after the cursor.
    std::vector<std::unique_ptr<FieldComparator>> &comparators = collector->queue_->comparators();
    for (size_t i = 0; i < comparators.size(); ++i) {
        if (auto set = comparators[i]->set_top_value(after->fields[i]); !set.has_value()) {
            return std::unexpected(std::move(set.error()));
        }
    }
    return collector;
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
    // The decision is per leaf: the reference implementation caches it from the
    // first leaf, which assumes every segment of the index is sorted the same way.
    const LeafReader *reader = context.reader();
    const Sort *index_sort = reader == nullptr ? nullptr : reader->leaf_meta_data().sort().get();
    std::unique_ptr<LeafCollector> collector(
            new Leaf(*this, context.doc_base(), std::move(comparators), can_early_terminate(sort_, index_sort)));
    return collector;
}

bool can_early_terminate(const Sort &search_sort, const Sort *index_sort) {
    const std::vector<SortField> &fields = search_sort.fields();
    // A sort by document number always allows it: the documents are already in
    // that order.
    if (!fields.empty() && fields.front().type() == SortFieldType::kDoc) {
        return true;
    }
    if (index_sort == nullptr) {
        return false;
    }
    const std::vector<SortField> &index_fields = index_sort->fields();
    if (fields.size() > index_fields.size()) {
        return false;
    }
    for (size_t i = 0; i < fields.size(); ++i) {
        if (!fields[i].equals(index_fields[i])) {
            return false;
        }
    }
    return true;
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

    return TopFieldDocs(TotalHits::create(static_cast<size_t>(total_hits_), total_hits_relation_), std::move(hits),
                        sort_);
}

} // namespace lucene
