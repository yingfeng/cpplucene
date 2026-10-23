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

module lucene_core:conjunction_bulk_scorer.impl;

import :conjunction_bulk_scorer;

import std;

import :bits;
import :bulk_scorer;
import :debug;
import :doc_id_set_iterator;
import :error;
import :leaf_collector;
import :scorable;
import :scorer;
import :types;

namespace lucene {

namespace {

/// The [`Scorable`] a collector sees while a conjunction is being collected.
class ConjunctionScorable final : public Scorable {
public:
    explicit ConjunctionScorable(ConjunctionBulkScorer &base) : base_(base) {}

    [[nodiscard]] Result<float> score() override {
        double sum = 0.0;
        const auto &indices = base_.all_scores_input_idx();
        const auto &scorers = base_.scorers();
        for (size_t i = 0; i < base_.required_scoring_len(); ++i) {
            auto child_score = scorers[indices[i]]->score();
            if (!child_score.has_value()) {
                return std::unexpected(std::move(child_score.error()));
            }
            sum += static_cast<double>(child_score.value());
        }
        return static_cast<float>(sum);
    }

    [[nodiscard]] Result<std::vector<ChildScorable>> get_children() override {
        // The children are reported in the order the clauses were given, not in
        // the cost-sorted order the scorer iterates them in.
        std::vector<ChildScorable> children;
        const auto &indices = base_.all_scores_input_idx();
        const auto &scorers = base_.scorers();
        children.reserve(indices.size());
        for (const size_t index : indices) {
            children.push_back(ChildScorable{scorers[index].get(), "MUST"});
        }
        return children;
    }

private:
    ConjunctionBulkScorer &base_;
};

} // namespace

ConjunctionBulkScorer::ConjunctionBulkScorer(std::vector<std::unique_ptr<Scorer>> all_scores,
                                             size_t required_scoring_len, std::vector<size_t> all_scores_input_idx)
        : all_scores_(std::move(all_scores)),
          required_scoring_len_(required_scoring_len),
          all_scores_input_idx_(std::move(all_scores_input_idx)) {}

Result<std::unique_ptr<ConjunctionBulkScorer>>
ConjunctionBulkScorer::create(std::vector<std::unique_ptr<Scorer>> required_scoring,
                              std::vector<std::unique_ptr<Scorer>> required_no_scoring) {
    const size_t required_scoring_len = required_scoring.size();
    const size_t num_clauses = required_scoring_len + required_no_scoring.size();
    if (num_clauses <= 1) {
        return err(LuceneError::illegal_argument("expected 2 or more clauses, got " + std::to_string(num_clauses)));
    }

    std::vector<std::unique_ptr<Scorer>> input;
    input.reserve(num_clauses);
    std::vector<std::pair<int64_t, size_t>> costs;
    costs.reserve(num_clauses);
    for (auto &scorer : required_scoring) {
        auto cost = scorer->cost();
        if (!cost.has_value()) {
            return std::unexpected(std::move(cost.error()));
        }
        costs.emplace_back(cost.value(), input.size());
        input.push_back(std::move(scorer));
    }
    for (auto &scorer : required_no_scoring) {
        auto cost = scorer->cost();
        if (!cost.has_value()) {
            return std::unexpected(std::move(cost.error()));
        }
        costs.emplace_back(cost.value(), input.size());
        input.push_back(std::move(scorer));
    }
    std::stable_sort(costs.begin(), costs.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

    std::vector<std::unique_ptr<Scorer>> all_scores;
    all_scores.reserve(num_clauses);
    std::vector<size_t> all_scores_input_idx(num_clauses, 0);
    for (const auto &[cost, index] : costs) {
        all_scores_input_idx[index] = all_scores.size();
        all_scores.push_back(std::move(input[index]));
    }

    std::unique_ptr<ConjunctionBulkScorer> scorer(
            new ConjunctionBulkScorer(std::move(all_scores), required_scoring_len, std::move(all_scores_input_idx)));
    return scorer;
}

Result<int32_t> ConjunctionBulkScorer::score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                             int32_t max) {
    Scorer &lead1 = clause(0);
    Scorer &lead2 = clause(1);
    debug_check(lead1.doc_id() >= lead2.doc_id(), "the lead clause must be ahead of the second clause");

    if (lead1.doc_id() < min) {
        auto advanced = lead1.advance(min);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
    }
    if (lead1.doc_id() >= max) {
        return lead1.doc_id();
    }
    int32_t lead1_doc_id = lead1.doc_id();
    const int32_t lead2_doc_id = lead2.doc_id();

    ConjunctionScorable scorable(*this);
    auto set = collector.set_scorer(scorable);
    if (!set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }
    // The competitive iterator only moves forward, so one instance serves the
    // whole call.
    auto competitive = collector.competitive_iterator();
    if (!competitive.has_value()) {
        return std::unexpected(std::move(competitive.error()));
    }
    DocIdSetIterator *competitive_iterator = competitive.value().get();

    // The invariant of the main loop is that the lead clause is ahead of the
    // second clause, which does not hold on the very first document, so that case
    // is handled separately.
    if (lead1_doc_id == lead2_doc_id) {
        const int32_t doc = lead1_doc_id;
        bool accepted = true;
        if (accept_docs != nullptr) {
            auto has = accept_docs->get(static_cast<size_t>(doc));
            if (!has.has_value()) {
                return std::unexpected(std::move(has.error()));
            }
            accepted = has.value();
        }
        if (accepted) {
            bool matched = true;
            for (size_t index = 1; index < all_scores_.size(); ++index) {
                Scorer &other = clause(index);
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
                        matched = false;
                        break;
                    }
                }
                debug_check(other.doc_id() == doc, "every clause must be positioned on the document");
            }
            if (matched && competitive_iterator != nullptr && competitive_iterator->doc_id() < doc) {
                auto next = competitive_iterator->advance(doc);
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
                if (next.value() != doc) {
                    auto advanced = lead1.advance(next.value());
                    if (!advanced.has_value()) {
                        return std::unexpected(std::move(advanced.error()));
                    }
                    matched = false;
                }
            }
            if (matched) {
                auto collected = collector.collect(doc, scorable);
                if (!collected.has_value()) {
                    return std::unexpected(std::move(collected.error()));
                }
                auto next = lead1.next_doc();
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
            }
            lead1_doc_id = lead1.doc_id();
        } else {
            auto next = lead1.next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            lead1_doc_id = lead1.doc_id();
        }
    }

    int32_t doc = lead1_doc_id;
    while (doc < max) {
        debug_check(lead2.doc_id() < doc, "the second clause must be behind the lead clause");

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

        // `lead2` is kept behind `lead1`, so it never matches here by accident.
        auto next2 = lead2.advance(doc);
        if (!next2.has_value()) {
            return std::unexpected(std::move(next2.error()));
        }
        if (next2.value() != doc) {
            auto advanced = lead1.advance(next2.value());
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
            doc = advanced.value();
            if (doc != next2.value()) {
                continue;
            }
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
        }
        debug_check(lead2.doc_id() == doc, "the second clause must be positioned on the document");

        bool restart = false;
        for (size_t index = 2; index < all_scores_.size(); ++index) {
            Scorer &other = clause(index);
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
        }
        if (restart) {
            continue;
        }
        if (competitive_iterator != nullptr && competitive_iterator->doc_id() < doc) {
            auto next = competitive_iterator->advance(doc);
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

        auto collected = collector.collect(doc, scorable);
        if (!collected.has_value()) {
            return std::unexpected(std::move(collected.error()));
        }
        auto next = lead1.next_doc();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        doc = next.value();
    }
    return lead1.doc_id();
}

Result<int64_t> ConjunctionBulkScorer::cost() { return all_scores_[0]->cost(); }

} // namespace lucene
