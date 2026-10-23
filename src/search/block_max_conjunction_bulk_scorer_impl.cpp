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

module lucene_core:block_max_conjunction_bulk_scorer.impl;

import :block_max_conjunction_bulk_scorer;

import std;

import :bits;
import :bulk_scorer;
import :debug;
import :doc_id_set_iterator;
import :error;
import :leaf_collector;
import :math_util;
import :scorable;
import :scorer;
import :types;

namespace lucene {

BlockMaxConjunctionBulkScorer::BlockMaxConjunctionBulkScorer(int32_t max_doc,
                                                             std::vector<std::unique_ptr<Scorer>> scorers)
        : scorable_(0.0F),
          sum_of_other_clauses_(scorers.size(), std::numeric_limits<double>::infinity()),
          max_doc_(max_doc),
          scorers_(std::move(scorers)) {}

Result<std::unique_ptr<BlockMaxConjunctionBulkScorer>>
BlockMaxConjunctionBulkScorer::create(int32_t max_doc, std::vector<std::unique_ptr<Scorer>> scorers) {
    if (scorers.empty()) {
        return err(LuceneError::illegal_argument("a block-max conjunction needs at least one scorer"));
    }
    std::vector<std::pair<size_t, int64_t>> costs;
    costs.reserve(scorers.size());
    for (size_t index = 0; index < scorers.size(); ++index) {
        auto cost = scorers[index]->cost();
        if (!cost.has_value()) {
            return std::unexpected(std::move(cost.error()));
        }
        costs.emplace_back(index, cost.value());
    }
    // The least costly clauses lead, and equal costs keep their input order.
    std::stable_sort(costs.begin(), costs.end(), [](const auto &a, const auto &b) { return a.second < b.second; });

    // Resolve the permutation in place, so that the scorers keep their
    // allocation.
    std::vector<size_t> destinations(scorers.size(), 0);
    for (size_t destination = 0; destination < costs.size(); ++destination) {
        destinations[costs[destination].first] = destination;
    }
    for (size_t source = 0; source < scorers.size(); ++source) {
        while (destinations[source] != source) {
            const size_t destination = destinations[source];
            std::swap(scorers[source], scorers[destination]);
            std::swap(destinations[source], destinations[destination]);
        }
    }

    std::unique_ptr<BlockMaxConjunctionBulkScorer> scorer(new BlockMaxConjunctionBulkScorer(max_doc, std::move(scorers)));
    return scorer;
}

Result<float> BlockMaxConjunctionBulkScorer::compute_max_score(int32_t window_min, int32_t window_max) {
    for (auto &scorer : scorers_) {
        auto shallow = scorer->advance_shallow(window_min);
        if (!shallow.has_value()) {
            return std::unexpected(std::move(shallow.error()));
        }
    }

    double max_window_score = 0.0;
    for (size_t index = 0; index < scorers_.size(); ++index) {
        auto max_clause_score = scorers_[index]->get_max_score(window_max);
        if (!max_clause_score.has_value()) {
            return std::unexpected(std::move(max_clause_score.error()));
        }
        sum_of_other_clauses_[index] = static_cast<double>(max_clause_score.value());
        max_window_score += sum_of_other_clauses_[index];
    }
    // Turn the per-clause maxima into suffix sums, so that the running score of
    // the clauses that were already confirmed can be completed with one add.
    for (size_t index = sum_of_other_clauses_.size(); index > 1; --index) {
        sum_of_other_clauses_[index - 2] += sum_of_other_clauses_[index - 1];
    }
    return static_cast<float>(max_window_score);
}

VoidResult BlockMaxConjunctionBulkScorer::score_window(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                                        int32_t max, float max_window_score) {
    if (max_window_score < scorable_.min_competitive_score) {
        // No hit of this window can be competitive.
        return {};
    }
    const int32_t scorers_len = static_cast<int32_t>(scorers_.size());

    Scorer &lead1 = *scorers_[0];
    if (lead1.doc_id() < min) {
        auto advanced = lead1.advance(min);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
    }
    int32_t doc = lead1.doc_id();
    const double sum_other_at_1 = sum_of_other_clauses_[1];

    for (;;) {
        if (doc >= max) {
            break;
        }
        if (accept_docs != nullptr) {
            auto has = accept_docs->get(static_cast<size_t>(doc));
            if (!has.has_value()) {
                return std::unexpected(std::move(has.error()));
            }
            if (!has.value()) {
                auto next = lead1.next_doc();
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
                doc = next.value();
                continue;
            }
        }

        // The score is computed as more clauses match, so that the remaining
        // clauses can be skipped when the total cannot become competitive.
        const bool has_min_comp = scorable_.min_competitive_score > 0.0F;
        double current_score = 0.0;
        if (has_min_comp) {
            auto lead1_score = lead1.score();
            if (!lead1_score.has_value()) {
                return std::unexpected(std::move(lead1_score.error()));
            }
            current_score = static_cast<double>(lead1_score.value());
            if (static_cast<float>(MathUtil::sum_upper_bound(current_score + sum_other_at_1, scorers_len)) <
                scorable_.min_competitive_score) {
                auto next = lead1.next_doc();
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
                doc = next.value();
                continue;
            }
        }

        // The second cheapest clause gets its own step, which keeps the loop
        // below simple.  It may already be on `doc` after a `continue`.
        {
            Scorer &lead2 = *scorers_[1];
            if (lead2.doc_id() < doc) {
                auto next = lead2.advance(doc);
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
                if (next.value() != doc) {
                    auto advanced = lead1.advance(next.value());
                    if (!advanced.has_value()) {
                        return std::unexpected(std::move(advanced.error()));
                    }
                    doc = advanced.value();
                    continue;
                }
            }
            debug_check(lead2.doc_id() == doc, "the second clause must be positioned on the document");
        }
        if (has_min_comp) {
            auto lead2_score = scorers_[1]->score();
            if (!lead2_score.has_value()) {
                return std::unexpected(std::move(lead2_score.error()));
            }
            current_score += static_cast<double>(lead2_score.value());
        }

        bool restart = false;
        for (size_t index = 2; index < scorers_.size(); ++index) {
            if (has_min_comp &&
                static_cast<float>(MathUtil::sum_upper_bound(current_score + sum_of_other_clauses_[index], scorers_len)) <
                        scorable_.min_competitive_score) {
                auto next = lead1.next_doc();
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
                doc = next.value();
                restart = true;
                break;
            }
            Scorer &other = *scorers_[index];
            if (other.doc_id() < doc) {
                auto next = other.advance(doc);
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
                if (next.value() != doc) {
                    auto advanced = lead1.advance(next.value());
                    if (!advanced.has_value()) {
                        return std::unexpected(std::move(advanced.error()));
                    }
                    doc = advanced.value();
                    restart = true;
                    break;
                }
            }
            debug_check(other.doc_id() == doc, "every clause must be positioned on the document");
            if (has_min_comp) {
                auto child_score = other.score();
                if (!child_score.has_value()) {
                    return std::unexpected(std::move(child_score.error()));
                }
                current_score += static_cast<double>(child_score.value());
            }
        }
        if (restart) {
            continue;
        }

        if (!has_min_comp) {
            for (auto &scorer : scorers_) {
                auto child_score = scorer->score();
                if (!child_score.has_value()) {
                    return std::unexpected(std::move(child_score.error()));
                }
                current_score += static_cast<double>(child_score.value());
            }
        }

        scorable_.current_score = static_cast<float>(current_score);
        auto collected = collector.collect(doc, scorable_);
        if (!collected.has_value()) {
            return std::unexpected(std::move(collected.error()));
        }

        if (max_window_score < scorable_.min_competitive_score) {
            // The collector raised the bar above the whole window.
            return {};
        }
        auto next = lead1.next_doc();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        doc = next.value();
    }
    return {};
}

Result<int32_t> BlockMaxConjunctionBulkScorer::score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                                     int32_t max) {
    auto set = collector.set_scorer(scorable_);
    if (!set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }

    int32_t window_min = std::max(scorers_[0]->doc_id(), min);
    while (window_min < max) {
        // The impacts of the cheapest clause decide the windows.
        auto shallow = scorers_[0]->advance_shallow(window_min);
        if (!shallow.has_value()) {
            return std::unexpected(std::move(shallow.error()));
        }
        // `window_max` is inclusive.
        const int32_t window_max = std::min(shallow.value(), max - 1);

        float max_window_score = std::numeric_limits<float>::infinity();
        if (scorable_.min_competitive_score > 0.0F) {
            auto computed = compute_max_score(window_min, window_max);
            if (!computed.has_value()) {
                return std::unexpected(std::move(computed.error()));
            }
            max_window_score = computed.value();
        }

        auto scored = score_window(collector, accept_docs, window_min, window_max + 1, max_window_score);
        if (!scored.has_value()) {
            return std::unexpected(std::move(scored.error()));
        }
        window_min = std::max(scorers_[0]->doc_id(), window_max + 1);
    }

    return window_min >= max_doc_ ? NO_MORE_DOCS : window_min;
}

Result<int64_t> BlockMaxConjunctionBulkScorer::cost() { return scorers_[0]->cost(); }

} // namespace lucene
