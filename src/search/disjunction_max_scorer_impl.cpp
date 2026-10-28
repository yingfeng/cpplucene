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

module lucene_core:disjunction_max_scorer.impl;

import :disjunction_max_scorer;

import std;

import :disjunction_disi_approximation;
import :disjunction_score_block_boundary_propagator;
import :doc_id_set_iterator;
import :error;
import :score_mode;
import :scorer;
import :two_phase_iterator;
import :types;

namespace lucene {

Result<std::unique_ptr<DisjunctionMaxScorer>>
DisjunctionMaxScorer::create(float tie_breaker_multiplier, std::vector<std::unique_ptr<Scorer>> sub_scorers,
                             ScoreMode score_mode) {
    // NaN passes neither comparison, which is what the reference implementation
    // relies on as well.
    if (tie_breaker_multiplier < 0.0F || tie_breaker_multiplier > 1.0F) {
        return err(LuceneError::illegal_argument("tieBreakerMultiplier must be in [0, 1]"));
    }
    if (sub_scorers.size() < 2) {
        return err(LuceneError::illegal_argument("There must be at least 2 sub-scorers, got " +
                                                 std::to_string(sub_scorers.size())));
    }
    std::vector<Scorer *> raw;
    raw.reserve(sub_scorers.size());
    for (const auto &scorer : sub_scorers) {
        if (scorer == nullptr) {
            return err(LuceneError::illegal_argument("DisjunctionMaxScorer sub-scorers must not be null"));
        }
        raw.push_back(scorer.get());
    }
    auto approximation = DisjunctionDISIApproximation::create(raw);
    if (!approximation.has_value()) {
        return std::unexpected(std::move(approximation.error()));
    }
    std::vector<Scorer *> propagator_scorers = raw;
    auto propagator = std::optional<DisjunctionScoreBlockBoundaryPropagator>{};
    if (score_mode == ScoreMode::TopScores) {
        // The propagator shallow-advances every clause once, so it has to be built
        // before the scorer hands them out.
        auto built = DisjunctionScoreBlockBoundaryPropagator::create(propagator_scorers);
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        propagator = std::move(built.value());
    }
    std::unique_ptr<DisjunctionMaxScorer> scorer(new DisjunctionMaxScorer(
            std::move(sub_scorers), std::move(raw), std::move(approximation.value()), tie_breaker_multiplier,
            score_mode));
    scorer->propagator_ = std::move(propagator);
    return scorer;
}

int32_t DisjunctionMaxScorer::doc_id() const { return disi_->doc_id(); }

Result<int32_t> DisjunctionMaxScorer::next_doc() { return disi_->next_doc(); }

Result<int32_t> DisjunctionMaxScorer::advance(int32_t target) { return disi_->advance(target); }

Result<int64_t> DisjunctionMaxScorer::cost() const { return disi_->cost(); }

Result<int32_t> DisjunctionMaxScorer::scoring_doc_id() { return disi_->doc_id(); }

Result<float> DisjunctionMaxScorer::get_max_score(int32_t upto) {
    float score_max = 0.0F;
    double other_score_sum = 0.0;
    for (auto &scorer : sub_scorers_) {
        auto scoring_doc = scorer->scoring_doc_id();
        if (!scoring_doc.has_value()) {
            return std::unexpected(std::move(scoring_doc.error()));
        }
        if (scoring_doc.value() > upto) {
            continue;
        }
        auto sub_score = scorer->get_max_score(upto);
        if (!sub_score.has_value()) {
            return std::unexpected(std::move(sub_score.error()));
        }
        if (sub_score.value() >= score_max) {
            other_score_sum += static_cast<double>(score_max);
            score_max = sub_score.value();
        } else {
            other_score_sum += static_cast<double>(sub_score.value());
        }
    }
    return static_cast<float>(static_cast<double>(score_max) + other_score_sum * tie_breaker_multiplier_);
}

TwoPhaseState DisjunctionMaxScorer::has_two_phase_iterator() const {
    for (const auto &scorer : sub_scorers_) {
        if (scorer->has_two_phase_iterator() == TwoPhaseState::Yes) {
            return TwoPhaseState::Yes;
        }
    }
    return TwoPhaseState::No;
}

Result<bool> DisjunctionMaxScorer::matches() {
    const int32_t doc = disi_->doc_id();
    for (auto &scorer : sub_scorers_) {
        if (scorer->doc_id() != doc) {
            continue;
        }
        if (scorer->has_two_phase_iterator() == TwoPhaseState::No) {
            // A sub-scorer that needs no confirmation already matches.
            return true;
        }
        auto matched = scorer->matches();
        if (!matched.has_value()) {
            return matched;
        }
        if (matched.value()) {
            return true;
        }
    }
    return false;
}

float DisjunctionMaxScorer::match_cost() const {
    float cost = 0.0F;
    for (const auto &scorer : sub_scorers_) {
        cost += scorer->match_cost();
    }
    return cost;
}

int32_t DisjunctionMaxScorer::approx_doc_id() const { return disi_->doc_id(); }

Result<int32_t> DisjunctionMaxScorer::approx_next_doc() { return disi_->next_doc(); }

Result<int32_t> DisjunctionMaxScorer::approx_advance(int32_t target) { return disi_->advance(target); }

Result<int64_t> DisjunctionMaxScorer::approx_cost() const { return disi_->cost(); }

Result<float> DisjunctionMaxScorer::score() {
    const int32_t doc = disi_->doc_id();
    float score_max = 0.0F;
    double other_score_sum = 0.0;
    for (auto &scorer : sub_scorers_) {
        if (scorer->doc_id() != doc) {
            continue;
        }
        auto sub_score = scorer->score();
        if (!sub_score.has_value()) {
            return std::unexpected(std::move(sub_score.error()));
        }
        if (sub_score.value() >= score_max) {
            other_score_sum += static_cast<double>(score_max);
            score_max = sub_score.value();
        } else {
            other_score_sum += static_cast<double>(sub_score.value());
        }
    }
    return static_cast<float>(static_cast<double>(score_max) + other_score_sum * tie_breaker_multiplier_);
}

Result<int32_t> DisjunctionMaxScorer::advance_shallow(int32_t target) {
    if (!propagator_.has_value()) {
        return NO_MORE_DOCS;
    }
    return propagator_->advance_shallow(target, raw_scorers_);
}

VoidResult DisjunctionMaxScorer::set_min_competitive_score(float min_score) {
    if (propagator_.has_value()) {
        // Raising the threshold drops the clauses that can no longer contribute a
        // competitive hit from the block boundary computation.
        propagator_->set_min_competitive_score(min_score);
    }
    for (auto &scorer : sub_scorers_) {
        if (auto set = scorer->set_min_competitive_score(min_score); !set.has_value()) {
            return set;
        }
    }
    return {};
}

} // namespace lucene
