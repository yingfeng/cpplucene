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

module lucene_core:disjunction_score_block_boundary_propagator.impl;

import :disjunction_score_block_boundary_propagator;

import std;

import :core_helper;
import :doc_id_set_iterator;
import :error;
import :scorer;
import :types;

namespace lucene {

Result<DisjunctionScoreBlockBoundaryPropagator>
DisjunctionScoreBlockBoundaryPropagator::create(std::span<Scorer *const> scorers) {
    DisjunctionScoreBlockBoundaryPropagator propagator;
    propagator.cost_.reserve(scorers.size());
    for (size_t index = 0; index < scorers.size(); ++index) {
        Scorer &scorer = *scorers[index];
        auto shallow = scorer.advance_shallow(0);
        if (!shallow.has_value()) {
            return std::unexpected(std::move(shallow.error()));
        }
        auto max_score = scorer.get_max_score(NO_MORE_DOCS);
        if (!max_score.has_value()) {
            return std::unexpected(std::move(max_score.error()));
        }
        auto cost = scorer.cost();
        if (!cost.has_value()) {
            return std::unexpected(std::move(cost.error()));
        }
        propagator.cost_.push_back(Cost{max_score.value(), index, cost.value()});
    }
    std::sort(propagator.cost_.begin(), propagator.cost_.end(), [](const Cost &a, const Cost &b) {
        const int32_t by_score = CoreHelper::compare_f32(a.max_score, b.max_score);
        if (by_score != 0) {
            return by_score < 0;
        }
        return a.iter_cost < b.iter_cost;
    });
    return propagator;
}

Result<int32_t> DisjunctionScoreBlockBoundaryPropagator::advance_shallow(int32_t target,
                                                                        std::span<Scorer *const> scorers) {
    // The clauses that cannot reach the minimum competitive score are only
    // propagated, without letting them shrink the block.
    for (size_t i = 0; i < lead_index_; ++i) {
        Scorer &scorer = *scorers[cost_[i].idx];
        auto doc = scorer.scoring_doc_id();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        if (doc.value() < target) {
            auto shallow = scorer.advance_shallow(target);
            if (!shallow.has_value()) {
                return std::unexpected(std::move(shallow.error()));
            }
        }
    }

    Scorer &lead_scorer = *scorers[cost_[lead_index_].idx];
    auto lead_doc = lead_scorer.scoring_doc_id();
    if (!lead_doc.has_value()) {
        return std::unexpected(std::move(lead_doc.error()));
    }
    auto lead_shallow = lead_scorer.advance_shallow(std::max(lead_doc.value(), target));
    if (!lead_shallow.has_value()) {
        return std::unexpected(std::move(lead_shallow.error()));
    }
    int32_t upto = lead_shallow.value();

    // The clauses that can still compete share the block boundary.
    for (size_t i = lead_index_ + 1; i < cost_.size(); ++i) {
        Scorer &scorer = *scorers[cost_[i].idx];
        auto doc = scorer.scoring_doc_id();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        if (doc.value() <= target) {
            auto shallow = scorer.advance_shallow(target);
            if (!shallow.has_value()) {
                return std::unexpected(std::move(shallow.error()));
            }
            upto = std::min(shallow.value(), upto);
        }
    }

    // The maximum-scoring clauses that are still beyond `target` bound the block
    // by their own document, which keeps the maximum score upper bound low.
    for (size_t i = cost_.size(); i > lead_index_ + 1; --i) {
        Scorer &scorer = *scorers[cost_[i - 1].idx];
        auto doc = scorer.scoring_doc_id();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        if (doc.value() > target) {
            upto = std::min(upto, doc.value() - 1);
        } else {
            break;
        }
    }

    return upto;
}

void DisjunctionScoreBlockBoundaryPropagator::set_min_competitive_score(float min_score) {
    while (lead_index_ + 1 < cost_.size() && min_score > cost_[lead_index_].max_score) {
        ++lead_index_;
    }
}

} // namespace lucene
