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

module lucene_core:boolean_scorer.impl;

import :boolean_scorer;

import std;

import :bits;
import :bulk_scorer;
import :doc_id_set_iterator;
import :doc_id_stream;
import :error;
import :leaf_collector;
import :priority_queue;
import :scorable;
import :scorer;
import :scorer_util;
import :types;

namespace lucene {

// ---------------------------------------------------------------------------
// BooleanScorer::Stream
// ---------------------------------------------------------------------------

VoidResult BooleanScorer::Stream::for_each(DocIdStreamConsumer &consumer) {
    for (size_t word_index = 0; word_index < BOOLEAN_SCORER_SET_SIZE; ++word_index) {
        uint64_t bits = scorer_->matching_[word_index];
        while (bits != 0) {
            const size_t index_in_window = (word_index << 6) | static_cast<size_t>(std::countr_zero(bits));
            const int32_t doc = base_ | static_cast<int32_t>(index_in_window);
            if (!scorer_->buckets_.empty()) {
                Bucket &bucket = scorer_->buckets_[index_in_window];
                if (bucket.freq >= scorer_->min_should_match_) {
                    if (auto consumed = consumer.accept_with_score(doc, bucket.score); !consumed.has_value()) {
                        return consumed;
                    }
                }
                bucket.freq = 0;
                bucket.score = 0.0F;
            } else {
                if (auto consumed = consumer.accept(doc); !consumed.has_value()) {
                    return consumed;
                }
            }
            bits &= bits - 1;
        }
        scorer_->matching_[word_index] = 0;
    }
    return {};
}

Result<int32_t> BooleanScorer::Stream::count(Scorable &scorer) {
    if (scorer_->min_should_match_ > 1) {
        return DocIdStream::count(scorer);
    }
    int32_t count = 0;
    for (const uint64_t bits : scorer_->matching_) {
        count += static_cast<int32_t>(std::popcount(bits));
    }
    return count;
}

// ---------------------------------------------------------------------------
// BooleanScorer
// ---------------------------------------------------------------------------

BooleanScorer::BooleanScorer(size_t min_should_match, bool needs_scores, bool with_buckets, int64_t cost)
    : buckets_(with_buckets ? std::vector<Bucket>(BOOLEAN_SCORER_SIZE) : std::vector<Bucket>()),
      head_(PriorityQueue<Clause, HeadCompare>::create(1, HeadCompare{}).value()),
      tail_(PriorityQueue<Clause, TailCompare>::create(1, TailCompare{}).value()),
      min_should_match_(min_should_match),
      cost_(cost),
      needs_scores_(needs_scores) {}

Result<std::unique_ptr<BooleanScorer>> BooleanScorer::create(std::vector<std::unique_ptr<Scorer>> scorers,
                                                             size_t min_should_match, bool needs_scores) {
    if (min_should_match < 1 || min_should_match > scorers.size()) {
        return err(LuceneError::illegal_argument(
                std::format("min_should_match should be within 1..num_scorers, got {}", min_should_match)));
    }
    if (scorers.size() <= 1) {
        return err(LuceneError::illegal_argument(
                std::format("this scorer can only be used with two scorers or more, got {}", scorers.size())));
    }

    const bool with_buckets = needs_scores || min_should_match > 1;
    const size_t head_size = scorers.size() - min_should_match + 1;
    const size_t tail_size = min_should_match - 1;

    auto head = PriorityQueue<Clause, HeadCompare>::create(head_size, HeadCompare{});
    if (!head.has_value()) {
        return std::unexpected(std::move(head.error()));
    }
    auto tail = PriorityQueue<Clause, TailCompare>::create(tail_size, TailCompare{});
    if (!tail.has_value()) {
        return std::unexpected(std::move(tail.error()));
    }

    std::unique_ptr<BooleanScorer> scorer(
            new BooleanScorer(min_should_match, needs_scores, with_buckets, /*cost=*/0));
    scorer->head_ = std::move(head.value());
    scorer->tail_ = std::move(tail.value());

    std::vector<int64_t> costs;
    costs.reserve(scorers.size());
    for (std::unique_ptr<Scorer> &clause_scorer : scorers) {
        auto cost = clause_scorer->cost();
        if (!cost.has_value()) {
            return std::unexpected(std::move(cost.error()));
        }
        const int32_t doc = clause_scorer->doc_id();
        Clause clause{std::move(clause_scorer), doc, cost.value()};
        costs.push_back(clause.cost);

        auto evicted = scorer->tail_.insert_with_overflow(std::move(clause));
        if (!evicted.has_value()) {
            return std::unexpected(std::move(evicted.error()));
        }
        if (evicted.value().has_value()) {
            if (auto added = scorer->head_.add(std::move(evicted.value().value())); !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
        }
    }

    auto computed = ScorerUtil::cost_with_min_should_match(costs, head_size + tail_size, min_should_match);
    if (!computed.has_value()) {
        return std::unexpected(std::move(computed.error()));
    }
    scorer->cost_ = computed.value();
    return scorer;
}

VoidResult BooleanScorer::score_clause_into_bitset(Clause &clause, const Bits *accept_docs, int32_t min,
                                                   int32_t max) {
    int32_t doc = clause.doc;
    if (doc < min) {
        auto advanced = clause.scorer->advance(min);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        doc = advanced.value();
    }
    while (doc < max) {
        bool accepted = true;
        if (accept_docs != nullptr) {
            auto is_accepted = accept_docs->get(static_cast<size_t>(doc));
            if (!is_accepted.has_value()) {
                return std::unexpected(std::move(is_accepted.error()));
            }
            accepted = is_accepted.value();
        }
        if (accepted) {
            const size_t index = static_cast<size_t>(doc) & BOOLEAN_SCORER_MASK;
            matching_[index >> 6] |= uint64_t{1} << (index & 0x3F);
            if (!buckets_.empty()) {
                Bucket &bucket = buckets_[index];
                ++bucket.freq;
                if (needs_scores_) {
                    auto value = clause.scorer->score();
                    if (!value.has_value()) {
                        return std::unexpected(std::move(value.error()));
                    }
                    bucket.score += value.value();
                }
            }
        }
        auto next = clause.scorer->next_doc();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        doc = next.value();
    }
    clause.doc = doc;
    return {};
}

Result<int32_t> BooleanScorer::advance(int32_t min) {
    // **Deviation:** the reference implementation empties the head's slot with
    // `take_top()` and puts the same clause back with `update_top_with_new_top()`,
    // which keeps the queue size.  The port's `PriorityQueue` has no
    // "take without removing" operation, so this walks with `pop`/`add`, which
    // visits the same clauses in the same order (the heap order is a function of
    // the keys, not of how an element was reinserted).
    auto head_top = head_.pop();
    if (!head_top.has_value()) {
        return std::unexpected(std::move(head_top.error()));
    }
    if (!head_top.value().has_value()) {
        return err(LuceneError::illegal_state("the head queue is empty"));
    }
    Clause top = std::move(head_top.value().value());

    while (top.doc < min) {
        auto tail_top = tail_.pop();
        if (!tail_top.has_value()) {
            return std::unexpected(std::move(tail_top.error()));
        }
        if (!tail_top.value().has_value()) {
            // No cheaper clause left: the head has to move on its own.
            auto advanced = top.scorer->advance(min);
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
            top.doc = advanced.value();
            if (auto added = head_.add(std::move(top)); !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
            auto again = head_.pop();
            if (!again.has_value()) {
                return std::unexpected(std::move(again.error()));
            }
            if (!again.value().has_value()) {
                return err(LuceneError::illegal_state("the head queue is empty after the update"));
            }
            top = std::move(again.value().value());
            continue;
        }

        Clause tail_clause = std::move(tail_top.value().value());
        if (top.cost <= tail_clause.cost) {
            // The head is the cheaper one: advance it and give the tail clause back.
            auto advanced = top.scorer->advance(min);
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
            top.doc = advanced.value();
            if (auto added = head_.add(std::move(top)); !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
            auto again = head_.pop();
            if (!again.has_value()) {
                return std::unexpected(std::move(again.error()));
            }
            if (!again.value().has_value()) {
                return err(LuceneError::illegal_state("the head queue is empty after the update"));
            }
            top = std::move(again.value().value());
            if (auto returned = tail_.add(std::move(tail_clause)); !returned.has_value()) {
                return std::unexpected(std::move(returned.error()));
            }
        } else {
            // The tail clause is cheaper: it takes the head's place for a while.
            auto advanced = tail_clause.scorer->advance(min);
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
            tail_clause.doc = advanced.value();
            if (auto added = head_.add(std::move(tail_clause)); !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
            if (auto returned = tail_.add(std::move(top)); !returned.has_value()) {
                return std::unexpected(std::move(returned.error()));
            }
            auto again = head_.pop();
            if (!again.has_value()) {
                return std::unexpected(std::move(again.error()));
            }
            if (!again.value().has_value()) {
                return err(LuceneError::illegal_state("the head queue is empty after the update"));
            }
            top = std::move(again.value().value());
        }
    }

    if (auto added = head_.add(std::move(top)); !added.has_value()) {
        return std::unexpected(std::move(added.error()));
    }
    const Clause *head = head_.top();
    if (head == nullptr) {
        return err(LuceneError::illegal_state("the head queue is empty"));
    }
    return head->doc;
}

VoidResult BooleanScorer::score_window_single_scorer(Clause &clause, LeafCollector &collector,
                                                     const Bits *accept_docs, int32_t window_min, int32_t window_max,
                                                     int32_t max) {
    const Clause *head = head_.top();
    if (head == nullptr) {
        return err(LuceneError::illegal_state("the head queue is empty"));
    }
    const int32_t next_window_base = head->doc & ~static_cast<int32_t>(BOOLEAN_SCORER_MASK);
    const int32_t end = std::max(window_max, std::min(max, next_window_base));

    int32_t doc = clause.doc;
    if (doc < window_min) {
        auto advanced = clause.scorer->advance(window_min);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        doc = advanced.value();
    }
    if (auto set = collector.set_scorer(*clause.scorer); !set.has_value()) {
        return set;
    }
    while (doc < end) {
        bool accepted = true;
        if (accept_docs != nullptr) {
            auto is_accepted = accept_docs->get(static_cast<size_t>(doc));
            if (!is_accepted.has_value()) {
                return std::unexpected(std::move(is_accepted.error()));
            }
            accepted = is_accepted.value();
        }
        if (accepted) {
            if (auto collected = collector.collect(doc, *clause.scorer); !collected.has_value()) {
                return collected;
            }
        }
        auto next = clause.scorer->next_doc();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        doc = next.value();
    }
    clause.doc = doc;
    // The general case scores through the window's accumulated score.
    return collector.set_scorer(score_);
}

VoidResult BooleanScorer::score_window_multiple_scorers(LeafCollector &collector, const Bits *accept_docs,
                                                        int32_t window_base, int32_t window_min, int32_t window_max,
                                                        size_t max_freq, std::vector<Clause> &leads) {
    while (max_freq < min_should_match_ && max_freq + tail_.size() >= min_should_match_) {
        // A match is still possible: pull the cheapest clause of the tail.
        auto candidate = tail_.pop();
        if (!candidate.has_value()) {
            return std::unexpected(std::move(candidate.error()));
        }
        if (!candidate.value().has_value()) {
            return err(LuceneError::illegal_state("the tail queue is empty"));
        }
        Clause clause = std::move(candidate.value().value());

        if (clause.doc < window_min) {
            auto advanced = clause.scorer->advance(window_min);
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
            clause.doc = advanced.value();
        }
        if (clause.doc < window_max) {
            leads.push_back(std::move(clause));
            ++max_freq;
        } else if (auto added = head_.add(std::move(clause)); !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
    }

    if (max_freq >= min_should_match_) {
        // The tail may hold further matches for this window.
        for (Clause &clause : tail_.take_all()) {
            leads.push_back(std::move(clause));
            ++max_freq;
        }

        for (Clause &clause : leads) {
            if (auto written = score_clause_into_bitset(clause, accept_docs, window_min, window_max);
                !written.has_value()) {
                return written;
            }
        }

        Stream stream(*this, window_base);
        if (auto collected = collector.collect_stream(stream, score_); !collected.has_value()) {
            return collected;
        }
    }

    return restore_leads(leads);
}

VoidResult BooleanScorer::restore_leads(std::vector<Clause> &leads) {
    for (Clause &clause : leads) {
        auto evicted = head_.insert_with_overflow(std::move(clause));
        if (!evicted.has_value()) {
            return std::unexpected(std::move(evicted.error()));
        }
        if (evicted.value().has_value()) {
            if (auto added = tail_.add(std::move(evicted.value().value())); !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
        }
    }
    leads.clear();
    return {};
}

Result<BooleanScorer::Clause *> BooleanScorer::score_window(int32_t top_doc, LeafCollector &collector,
                                                            const Bits *accept_docs, int32_t min, int32_t max) {
    const int32_t window_base = top_doc & ~static_cast<int32_t>(BOOLEAN_SCORER_MASK);
    const int32_t window_min = std::max(min, window_base);
    const int32_t window_max = std::min(max, window_base + static_cast<int32_t>(BOOLEAN_SCORER_SIZE));

    leads_.clear();
    auto head_top = head_.pop();
    if (!head_top.has_value()) {
        return std::unexpected(std::move(head_top.error()));
    }
    if (!head_top.value().has_value()) {
        return err(LuceneError::illegal_state("the head queue is empty"));
    }
    leads_.push_back(std::move(head_top.value().value()));
    size_t max_freq = 1;

    while (head_.size() > 0) {
        const Clause *top = head_.top();
        if (top == nullptr) {
            break;
        }
        if (top->doc >= window_max) {
            break;
        }
        auto popped = head_.pop();
        if (!popped.has_value()) {
            return std::unexpected(std::move(popped.error()));
        }
        if (!popped.value().has_value()) {
            return err(LuceneError::illegal_state("the head queue is empty"));
        }
        leads_.push_back(std::move(popped.value().value()));
        ++max_freq;
    }

    if (min_should_match_ == 1 && max_freq == 1) {
        // Only one clause can match in this window: collect straight from it.
        Clause single = std::move(leads_.front());
        leads_.clear();
        if (auto collected = score_window_single_scorer(single, collector, accept_docs, window_min, window_max, max);
            !collected.has_value()) {
            return std::unexpected(std::move(collected.error()));
        }
        auto added = head_.add(std::move(single));
        if (!added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
        Clause *head = head_.top_mut();
        if (head == nullptr) {
            return err(LuceneError::illegal_state("the head queue is empty"));
        }
        return head;
    }

    if (auto collected = score_window_multiple_scorers(collector, accept_docs, window_base, window_min, window_max,
                                                       max_freq, leads_);
        !collected.has_value()) {
        return std::unexpected(std::move(collected.error()));
    }
    Clause *head = head_.top_mut();
    if (head == nullptr) {
        return err(LuceneError::illegal_state("the head queue is empty"));
    }
    return head;
}

Result<int32_t> BooleanScorer::score(LeafCollector &collector, const Bits *accept_docs, int32_t min, int32_t max) {
    if (auto set = collector.set_scorer(score_); !set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }
    auto top = advance(min);
    if (!top.has_value()) {
        return std::unexpected(std::move(top.error()));
    }
    int32_t doc = top.value();
    while (doc < max) {
        auto window = score_window(doc, collector, accept_docs, min, max);
        if (!window.has_value()) {
            return std::unexpected(std::move(window.error()));
        }
        doc = window.value()->doc;
    }
    return doc;
}

void BooleanScorer::reset_window() noexcept {
    for (uint64_t &word : matching_) {
        word = 0;
    }
    for (Bucket &bucket : buckets_) {
        bucket.freq = 0;
        bucket.score = 0.0F;
    }
}

} // namespace lucene
