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

module lucene_core:block_max_conjunction_scorer.impl;

import :block_max_conjunction_scorer;

import :core_helper;
import :debug;
import :doc_id_set_iterator;
import :error;
import :scorable;
import :scorer;
import :types;

import std;

namespace lucene {

BlockMaxConjunctionScorer::BlockMaxConjunctionScorer(std::vector<std::unique_ptr<Scorer>> scorers,
                                                     std::vector<std::pair<size_t, float>> two_phase_children,
                                                     float match_cost)
        : scorers_(std::move(scorers)),
          two_phase_children_(std::move(two_phase_children)),
          match_cost_(match_cost),
          two_phase_(!two_phase_children_.empty()) {}

Result<std::unique_ptr<BlockMaxConjunctionScorer>>
BlockMaxConjunctionScorer::prepare(std::vector<std::unique_ptr<Scorer>> scorers) {
    if (scorers.empty()) {
        return err(LuceneError::illegal_argument("a block-max conjunction needs at least one scorer"));
    }

    // Lead with the lowest-cost scorer, preserving the input order for equal
    // costs.
    std::vector<std::pair<int64_t, size_t>> by_cost;
    by_cost.reserve(scorers.size());
    for (size_t index = 0; index < scorers.size(); ++index) {
        auto cost = scorers[index]->cost();
        if (!cost.has_value()) {
            return std::unexpected(std::move(cost.error()));
        }
        by_cost.emplace_back(cost.value(), index);
    }
    std::stable_sort(by_cost.begin(), by_cost.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

    std::vector<std::unique_ptr<Scorer>> sorted;
    sorted.reserve(scorers.size());
    for (const auto &[cost, index] : by_cost) {
        auto shallow = scorers[index]->advance_shallow(0);
        if (!shallow.has_value()) {
            return std::unexpected(std::move(shallow.error()));
        }
        sorted.push_back(std::move(scorers[index]));
    }

    // The clauses that need a two-phase confirmation are checked cheapest first.
    std::vector<std::pair<size_t, float>> two_phase_children;
    for (size_t index = 0; index < sorted.size(); ++index) {
        if (sorted[index]->has_two_phase_iterator() == TwoPhaseState::Yes) {
            two_phase_children.emplace_back(index, sorted[index]->match_cost());
        }
    }
    std::sort(two_phase_children.begin(), two_phase_children.end(), [](const auto &a, const auto &b) {
        return CoreHelper::compare_f32(a.second, b.second) < 0;
    });
    double total_cost = 0.0;
    for (const auto &[index, cost] : two_phase_children) {
        total_cost += static_cast<double>(cost);
    }

    std::unique_ptr<BlockMaxConjunctionScorer> scorer(new BlockMaxConjunctionScorer(
            std::move(sorted), std::move(two_phase_children), static_cast<float>(total_cost)));
    return scorer;
}

int32_t BlockMaxConjunctionScorer::doc_id() const { return scorers_[0]->doc_id(); }

Result<int32_t> BlockMaxConjunctionScorer::next_doc() {
    const int32_t doc = doc_id();
    if (doc == NO_MORE_DOCS) {
        return NO_MORE_DOCS;
    }
    return advance(doc + 1);
}

Result<int32_t> BlockMaxConjunctionScorer::advance(int32_t target) {
    auto doc = advance_target(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    auto lead = scorers_[0]->approx_advance(doc.value());
    if (!lead.has_value()) {
        return std::unexpected(std::move(lead.error()));
    }
    return do_next(lead.value());
}

Result<int64_t> BlockMaxConjunctionScorer::cost() const { return scorers_[0]->cost(); }

Result<int32_t> BlockMaxConjunctionScorer::scoring_doc_id() { return scorer_doc_id(); }

Result<int32_t> BlockMaxConjunctionScorer::scorer_doc_id() { return scorers_[0]->scoring_doc_id(); }

Result<int32_t> BlockMaxConjunctionScorer::advance_shallow(int32_t target) {
    // The block boundaries of the lead scorer decide where the conjunction can
    // stop; the other clauses are shallow-advanced anyway, so that their score
    // upper bounds are tight.
    auto result = scorers_[0]->advance_shallow(target);
    if (!result.has_value()) {
        return std::unexpected(std::move(result.error()));
    }
    for (size_t index = 1; index < scorers_.size(); ++index) {
        auto shallow = scorers_[index]->advance_shallow(target);
        if (!shallow.has_value()) {
            return std::unexpected(std::move(shallow.error()));
        }
    }
    return result;
}

Result<float> BlockMaxConjunctionScorer::get_max_score(int32_t upto) {
    double sum = 0.0;
    for (auto &scorer : scorers_) {
        auto max_score = scorer->get_max_score(upto);
        if (!max_score.has_value()) {
            return std::unexpected(std::move(max_score.error()));
        }
        sum += static_cast<double>(max_score.value());
    }
    return static_cast<float>(sum);
}

VoidResult BlockMaxConjunctionScorer::move_to_next_block(int32_t target) {
    if (min_score_ == 0.0F) {
        // No threshold yet: the whole block is competitive.
        upto_ = target;
        max_score_ = std::numeric_limits<float>::infinity();
        return {};
    }
    auto shallow = advance_shallow(target);
    if (!shallow.has_value()) {
        return std::unexpected(std::move(shallow.error()));
    }
    upto_ = shallow.value();
    auto max_score = get_max_score(upto_);
    if (!max_score.has_value()) {
        return std::unexpected(std::move(max_score.error()));
    }
    max_score_ = max_score.value();
    return {};
}

Result<int32_t> BlockMaxConjunctionScorer::advance_target(int32_t target) {
    if (target > upto_) {
        auto moved = move_to_next_block(target);
        if (!moved.has_value()) {
            return std::unexpected(std::move(moved.error()));
        }
    }

    for (;;) {
        if (upto_ < target) {
            // **Deviation:** the reference implementation asserts here; answering
            // "nothing competitive is left" keeps a source that reports a short
            // last block from aborting.
            return NO_MORE_DOCS;
        }
        if (max_score_ >= min_score_) {
            return target;
        }
        if (upto_ == NO_MORE_DOCS) {
            return NO_MORE_DOCS;
        }
        target = upto_ + 1;
        auto moved = move_to_next_block(target);
        if (!moved.has_value()) {
            return std::unexpected(std::move(moved.error()));
        }
    }
}

Result<int32_t> BlockMaxConjunctionScorer::do_next(int32_t doc) {
    for (;;) {
        debug_check(doc == scorers_[0]->approx_doc_id(), "the lead scorer must be positioned on the document");

        if (doc == NO_MORE_DOCS) {
            return NO_MORE_DOCS;
        }

        if (doc > upto_) {
            // The scorers may report blocks that do not actually have any
            // matches, so the document has to be checked against the block the
            // conjunction is in.
            auto next_target = advance_target(doc);
            if (!next_target.has_value()) {
                return std::unexpected(std::move(next_target.error()));
            }
            if (next_target.value() != doc) {
                auto advanced = scorers_[0]->approx_advance(next_target.value());
                if (!advanced.has_value()) {
                    return std::unexpected(std::move(advanced.error()));
                }
                doc = advanced.value();
                continue;
            }
        }

        debug_check(doc <= upto_, "the document must be inside the current block");

        // Then look for agreement with the other iterations.
        bool restart = false;
        for (size_t index = 1; index < scorers_.size(); ++index) {
            Scorer &other = *scorers_[index];
            // `other` may already be on `doc` when the lead scorer exactly
            // matched on a previous round.
            if (other.approx_doc_id() < doc) {
                auto next = other.approx_advance(doc);
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
                if (next.value() > doc) {
                    // This clause is beyond the current document, which means the
                    // conjunction cannot match it: advance the lead scorer to the
                    // new highest document and start over.
                    auto target = advance_target(next.value());
                    if (!target.has_value()) {
                        return std::unexpected(std::move(target.error()));
                    }
                    auto advanced = scorers_[0]->approx_advance(target.value());
                    if (!advanced.has_value()) {
                        return std::unexpected(std::move(advanced.error()));
                    }
                    doc = advanced.value();
                    restart = true;
                    break;
                }
            }
            debug_check(other.approx_doc_id() == doc, "every clause must be positioned on the document");
        }
        if (restart) {
            continue;
        }
        return doc;
    }
}

Result<bool> BlockMaxConjunctionScorer::matches() {
    if (!two_phase_) {
        return true;
    }
    const int32_t doc = scorers_[0]->approx_doc_id();
    for (const auto &[index, cost] : two_phase_children_) {
        debug_check(scorers_[index]->approx_doc_id() == doc, "every confirmed clause must be positioned on the document");
        auto confirmed = scorers_[index]->matches();
        if (!confirmed.has_value()) {
            return std::unexpected(std::move(confirmed.error()));
        }
        if (!confirmed.value()) {
            return false;
        }
    }
    return true;
}

Result<float> BlockMaxConjunctionScorer::score() {
    double sum = 0.0;
    for (auto &scorer : scorers_) {
        auto child_score = scorer->score();
        if (!child_score.has_value()) {
            return std::unexpected(std::move(child_score.error()));
        }
        sum += static_cast<double>(child_score.value());
    }
    return static_cast<float>(sum);
}

VoidResult BlockMaxConjunctionScorer::set_min_competitive_score(float min_score) {
    min_score_ = min_score;
    return {};
}

Result<std::vector<ChildScorable>> BlockMaxConjunctionScorer::get_children() {
    std::vector<ChildScorable> children;
    children.reserve(scorers_.size());
    for (auto &scorer : scorers_) {
        children.push_back(ChildScorable{scorer.get(), "MUST"});
    }
    return children;
}

} // namespace lucene
