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

module lucene_core:max_score_bulk_scorer.impl;

import :max_score_bulk_scorer;

import std;

import :bits;
import :bulk_scorer;
import :core_helper;
import :debug;
import :disi_priority_queue;
import :disi_wrapper;
import :doc_id_set_iterator;
import :error;
import :leaf_collector;
import :math_util;
import :scorable;
import :scorer;
import :types;

namespace lucene {

MaxScoreBulkScorer::MaxScoreBulkScorer(int32_t max_doc, std::vector<std::unique_ptr<Scorer>> scorers,
                                       std::unique_ptr<Scorer> filter)
        : max_doc_(max_doc),
          essential_queue_(scorers.size()),
          window_matches_(static_cast<size_t>(kMaxScoreInnerWindowSize) >> 6, 0),
          window_scores_(static_cast<size_t>(kMaxScoreInnerWindowSize), 0.0) {
    all_scorers_.reserve(scorers.size());
    all_scorers_idx_.reserve(scorers.size());
    for (size_t index = 0; index < scorers.size(); ++index) {
        // The cost is read once, when the wrapper is built.
        const auto cost = scorers[index]->cost();
        const int64_t clause_cost = cost.has_value() ? cost.value() : 0;
        cost_ += clause_cost;
        all_scorers_.emplace_back(std::move(scorers[index]), clause_cost, 0.0F);
        all_scorers_idx_.push_back(index);
    }
    scratch_.assign(all_scorers_idx_.size(), 0);
    max_score_sums_.assign(all_scorers_idx_.size(), 0.0);
    if (filter != nullptr) {
        const auto cost = filter->cost().value_or(0);
        filter_ = std::make_unique<DisiWrapper<std::unique_ptr<Scorer>>>(std::move(filter), cost, 0.0F);
    }
}

Result<std::unique_ptr<MaxScoreBulkScorer>>
MaxScoreBulkScorer::create(int32_t max_doc, std::vector<std::unique_ptr<Scorer>> scorers,
                           std::unique_ptr<Scorer> filter) {
    if (scorers.empty()) {
        return err(LuceneError::illegal_argument("a max-score bulk scorer needs at least one scorer"));
    }
    std::unique_ptr<MaxScoreBulkScorer> scorer(
            new MaxScoreBulkScorer(max_doc, std::move(scorers), std::move(filter)));
    return scorer;
}

VoidResult MaxScoreBulkScorer::update_max_window_scores(int32_t window_min, int32_t window_max) {
    for (const size_t index : all_scorers_idx_) {
        auto &wrapper = all_scorers_[index];
        if (wrapper.doc() < window_max) {
            if (wrapper.doc() < window_min) {
                // A shallow advance gives the best possible upper bound.
                auto shallow = wrapper.scorer()->advance_shallow(window_min);
                if (!shallow.has_value()) {
                    return std::unexpected(std::move(shallow.error()));
                }
            }
            auto max_score = wrapper.scorer()->get_max_score(window_max - 1);
            if (!max_score.has_value()) {
                return std::unexpected(std::move(max_score.error()));
            }
            wrapper.set_max_window_score(max_score.value());
        } else {
            // This clause has no document in the window.
            wrapper.set_max_window_score(0.0F);
        }
    }
    return {};
}

Result<bool> MaxScoreBulkScorer::partition_scorers() {
    scratch_ = all_scorers_idx_;
    std::sort(scratch_.begin(), scratch_.end(), [this](size_t left, size_t right) {
        const auto &w1 = all_scorers_[left];
        const auto &w2 = all_scorers_[right];
        const double s1 = static_cast<double>(w1.max_window_score()) / static_cast<double>(std::max<int64_t>(w1.cost(), 1));
        const double s2 = static_cast<double>(w2.max_window_score()) / static_cast<double>(std::max<int64_t>(w2.cost(), 1));
        return CoreHelper::compare_f64(s1, s2) < 0;
    });

    double max_score_sum = 0.0;
    first_essential_scorer_ = 0;
    next_min_competitive_score_ = std::numeric_limits<float>::infinity();
    const size_t n = all_scorers_idx_.size();

    for (size_t idx = 0; idx < n; ++idx) {
        const size_t index = scratch_[idx];
        const auto &wrapper = all_scorers_[index];
        const double new_max_score_sum = max_score_sum + static_cast<double>(wrapper.max_window_score());
        const int32_t num_values = static_cast<int32_t>(first_essential_scorer_) + 1;
        const float max_score_sum_float = static_cast<float>(MathUtil::sum_upper_bound(new_max_score_sum, num_values));

        if (max_score_sum_float < scorable_.min_competitive_score) {
            max_score_sum = new_max_score_sum;
            all_scorers_idx_[first_essential_scorer_] = index;
            max_score_sums_[first_essential_scorer_] = max_score_sum;
            ++first_essential_scorer_;
        } else {
            const size_t position = n - 1 - (idx - first_essential_scorer_);
            all_scorers_idx_[position] = index;
            next_min_competitive_score_ = CoreHelper::min_f32(max_score_sum_float, next_min_competitive_score_);
        }
    }

    first_required_scorer_ = n;

    if (first_essential_scorer_ == n) {
        // Every clause is non-essential: no document of the window is competitive.
        return false;
    }

    essential_queue_.clear();
    for (size_t i = first_essential_scorer_; i < n; ++i) {
        essential_queue_.add(all_scorers_idx_[i], all_scorers_);
    }

    if (first_essential_scorer_ == n - 1) {
        // With a single essential clause, a hit that skipped the best
        // non-essential clause may still be competitive, which makes the best
        // non-essential clauses required.
        first_required_scorer_ = n - 1;
        double max_required_score = static_cast<double>(
                all_scorers_[all_scorers_idx_[first_essential_scorer_]].max_window_score());
        while (first_required_scorer_ > 0) {
            double max_possible_score_without_previous = max_required_score;
            if (first_required_scorer_ > 1) {
                max_possible_score_without_previous += max_score_sums_[first_required_scorer_ - 2];
            }
            if (static_cast<float>(max_possible_score_without_previous) >= scorable_.min_competitive_score) {
                break;
            }
            --first_required_scorer_;
            max_required_score += static_cast<double>(
                    all_scorers_[all_scorers_idx_[first_required_scorer_]].max_window_score());
        }
    }

    return true;
}

Result<int32_t> MaxScoreBulkScorer::compute_outer_window_max(int32_t window_min) {
    const size_t all_scorers_len = all_scorers_idx_.size();
    const size_t first_window_lead = std::min(first_essential_scorer_, all_scorers_len - 1);

    int32_t window_max = NO_MORE_DOCS;
    for (size_t i = first_window_lead; i < all_scorers_len; ++i) {
        const size_t index = all_scorers_idx_[i];
        auto &wrapper = all_scorers_[index];
        const bool eligible = filter_ == nullptr || wrapper.cost() >= filter_->cost();
        if (!eligible) {
            continue;
        }
        auto upto = wrapper.scorer()->advance_shallow(std::max(wrapper.doc(), window_min));
        if (!upto.has_value()) {
            return std::unexpected(std::move(upto.error()));
        }
        // `upto` is inclusive.
        window_max = static_cast<int32_t>(std::min<int64_t>(window_max, static_cast<int64_t>(upto.value()) + 1));
    }

    if (all_scorers_len - first_window_lead > 1) {
        // Amortize the cost of computing maximum scores over at least 32
        // candidate matches per clause and per window.
        const int32_t threshold = num_outer_windows_ * 32 * static_cast<int32_t>(all_scorers_len);
        if (num_candidates_ < threshold) {
            min_window_size_ = std::min(min_window_size_ << 1, kMaxScoreInnerWindowSize);
        } else {
            min_window_size_ = 1;
        }
        const int32_t min_window_max =
                static_cast<int32_t>(std::min<int64_t>(std::numeric_limits<int32_t>::max(),
                                                       static_cast<int64_t>(window_min) + min_window_size_));
        window_max = std::max(window_max, min_window_max);
    }
    return window_max;
}

VoidResult MaxScoreBulkScorer::score_non_essential_clauses(LeafCollector &collector, int32_t doc,
                                                           double essential_score,
                                                           size_t num_non_essential_clauses) {
    ++num_candidates_;

    double score = essential_score;
    for (size_t i = num_non_essential_clauses; i > 0; --i) {
        const size_t slot = i - 1;
        const float max_possible_score = static_cast<float>(MathUtil::sum_upper_bound(
                score + max_score_sums_[slot], static_cast<int32_t>(all_scorers_idx_.size())));
        if (max_possible_score < scorable_.min_competitive_score) {
            // The hit is not competitive.
            return {};
        }

        const size_t index = all_scorers_idx_[slot];
        auto &wrapper = all_scorers_[index];
        if (wrapper.doc() < doc) {
            auto advanced = wrapper.scorer()->advance(doc);
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
            wrapper.set_doc(advanced.value());
        }
        if (wrapper.doc() == doc) {
            auto child_score = wrapper.scorer()->score();
            if (!child_score.has_value()) {
                return std::unexpected(std::move(child_score.error()));
            }
            score += static_cast<double>(child_score.value());
        }
    }

    scorable_.current_score = static_cast<float>(score);
    return collector.collect(doc, scorable_);
}

VoidResult MaxScoreBulkScorer::score_inner_window(LeafCollector &collector, const Bits *accept_docs, int32_t max) {
    const auto top_index_opt = essential_queue_.top();
    if (!top_index_opt.has_value()) {
        return err(LuceneError::illegal_state("the essential queue is empty"));
    }
    size_t top_index = top_index_opt.value();
    int32_t inner_window_min = all_scorers_[top_index].doc();
    int32_t inner_window_max = static_cast<int32_t>(
            std::min<int64_t>(max, static_cast<int64_t>(inner_window_min) + kMaxScoreInnerWindowSize));

    // Collect the matches of the essential clauses into a bit set, which saves
    // rebalancing the queue on every match.
    for (;;) {
        auto &wrapper = all_scorers_[top_index];
        int32_t doc = wrapper.doc();
        while (doc < inner_window_max) {
            bool accepted = true;
            if (accept_docs != nullptr) {
                auto has = accept_docs->get(static_cast<size_t>(doc));
                if (!has.has_value()) {
                    return std::unexpected(std::move(has.error()));
                }
                accepted = has.value();
            }
            if (accepted) {
                const size_t offset = static_cast<size_t>(doc - inner_window_min);
                window_matches_[offset >> 6] |= 1ULL << (offset & 0x3F);
                auto child_score = wrapper.scorer()->score();
                if (!child_score.has_value()) {
                    return std::unexpected(std::move(child_score.error()));
                }
                window_scores_[offset] += static_cast<double>(child_score.value());
            }
            auto next = wrapper.scorer()->next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            doc = next.value();
        }

        wrapper.set_doc(doc);
        top_index = essential_queue_.update_top(all_scorers_);
        if (all_scorers_[top_index].doc() >= inner_window_max) {
            break;
        }
    }

    for (size_t word_index = 0; word_index < window_matches_.size(); ++word_index) {
        uint64_t bits = window_matches_[word_index];
        window_matches_[word_index] = 0;
        while (bits != 0) {
            const size_t ntz = static_cast<size_t>(__builtin_ctzll(bits));
            bits ^= 1ULL << ntz;
            const size_t offset = (word_index << 6) | ntz;
            const int32_t doc = inner_window_min + static_cast<int32_t>(offset);

            // The optional filter only decides whether the candidate is a hit;
            // it never contributes to the score.
            if (filter_ != nullptr) {
                if (filter_->doc() < doc) {
                    auto advanced = filter_->scorer()->advance(doc);
                    if (!advanced.has_value()) {
                        return std::unexpected(std::move(advanced.error()));
                    }
                    filter_->set_doc(advanced.value());
                }
                if (filter_->doc() != doc) {
                    window_scores_[offset] = 0.0;
                    continue;
                }
            }

            const double score = window_scores_[offset];
            window_scores_[offset] = 0.0;
            if (auto scored = score_non_essential_clauses(collector, doc, score, first_essential_scorer_);
                !scored.has_value()) {
                return std::unexpected(std::move(scored.error()));
            }
        }
    }
    return {};
}

int32_t MaxScoreBulkScorer::next_candidate(int32_t range_end) const {
    if (range_end >= max_doc_) {
        return NO_MORE_DOCS;
    }
    int32_t next = NO_MORE_DOCS;
    for (const size_t index : all_scorers_idx_) {
        const auto &wrapper = all_scorers_[index];
        if (wrapper.doc() < range_end) {
            return range_end;
        }
        next = std::min(next, wrapper.doc());
    }
    return next;
}

Result<int32_t> MaxScoreBulkScorer::score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                          int32_t max) {
    auto set = collector.set_scorer(scorable_);
    if (!set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }

    int32_t outer_window_min = min;

    while (outer_window_min < max) {
        auto computed = compute_outer_window_max(outer_window_min);
        if (!computed.has_value()) {
            return std::unexpected(std::move(computed.error()));
        }
        int32_t outer_window_max = std::min(computed.value(), max);

        for (;;) {
            auto updated = update_max_window_scores(outer_window_min, outer_window_max);
            if (!updated.has_value()) {
                return std::unexpected(std::move(updated.error()));
            }

            auto partitioned = partition_scorers();
            if (!partitioned.has_value()) {
                return std::unexpected(std::move(partitioned.error()));
            }
            if (!partitioned.value()) {
                // No matches in this window.
                outer_window_min = outer_window_max;
                break;
            }

            // Windows and maximum scores depend on each other, so the window is
            // recomputed from the essential clauses of the previous window until
            // it stops shrinking.
            auto recomputed = compute_outer_window_max(outer_window_min);
            if (!recomputed.has_value()) {
                return std::unexpected(std::move(recomputed.error()));
            }
            if (recomputed.value() >= outer_window_max) {
                break;
            }
            outer_window_max = recomputed.value();
        }
        if (outer_window_min >= outer_window_max) {
            continue;
        }

        // Move the essential clauses into the window.
        auto top_opt = essential_queue_.top();
        if (!top_opt.has_value()) {
            return err(LuceneError::illegal_state("the essential queue is empty"));
        }
        size_t top_index = top_opt.value();
        while (all_scorers_[top_index].doc() < outer_window_min) {
            auto advanced = all_scorers_[top_index].scorer()->advance(outer_window_min);
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
            all_scorers_[top_index].set_doc(advanced.value());
            top_index = essential_queue_.update_top(all_scorers_);
        }

        int32_t top_doc = all_scorers_[top_index].doc();
        while (top_doc < outer_window_max) {
            auto scored = score_inner_window(collector, accept_docs, outer_window_max);
            if (!scored.has_value()) {
                return std::unexpected(std::move(scored.error()));
            }
            auto next_top = essential_queue_.top();
            if (!next_top.has_value()) {
                return err(LuceneError::illegal_state("the essential queue is empty"));
            }
            top_doc = all_scorers_[next_top.value()].doc();
            if (top_doc == NO_MORE_DOCS) {
                // Every essential clause is exhausted.
                return next_candidate(max);
            }

            if (scorable_.min_competitive_score >= next_min_competitive_score_) {
                // The minimum competitive score rose enough for a better
                // partitioning.
                break;
            }
        }
        const int32_t previous_window_min = outer_window_min;
        outer_window_min = std::min(top_doc, outer_window_max);
        if (outer_window_min <= previous_window_min) {
            // No progress: stop rather than spinning on an exhausted window.
            return next_candidate(max);
        }
        ++num_outer_windows_;
    }

    return next_candidate(max);
}

Result<int64_t> MaxScoreBulkScorer::cost() { return cost_; }

} // namespace lucene
