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

module lucene_core:disjunction_scorer.impl;

import :disjunction_scorer;

import :disjunction_disi_approximation;
import :doc_id_set_iterator;
import :error;
import :scorer;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<DisjunctionSumScorer>>
DisjunctionSumScorer::create(std::vector<std::unique_ptr<Scorer>> sub_scorers) {
    if (sub_scorers.size() < 2) {
        return err(LuceneError::illegal_argument("There must be at least 2 sub-scorers, got " +
                                                 std::to_string(sub_scorers.size())));
    }
    std::vector<Scorer *> raw;
    raw.reserve(sub_scorers.size());
    for (const auto &scorer : sub_scorers) {
        if (scorer == nullptr) {
            return err(LuceneError::illegal_argument("DisjunctionSumScorer sub-scorers must not be null"));
        }
        raw.push_back(scorer.get());
    }
    auto approximation = DisjunctionDISIApproximation::create(raw);
    if (!approximation.has_value()) {
        return std::unexpected(std::move(approximation.error()));
    }
    return std::unique_ptr<DisjunctionSumScorer>(
            new DisjunctionSumScorer(std::move(sub_scorers), std::move(approximation.value())));
}

int32_t DisjunctionSumScorer::doc_id() const { return disi_->doc_id(); }

Result<int32_t> DisjunctionSumScorer::next_doc() { return disi_->next_doc(); }

Result<int32_t> DisjunctionSumScorer::advance(int32_t target) { return disi_->advance(target); }

Result<int64_t> DisjunctionSumScorer::cost() const { return disi_->cost(); }

Result<int32_t> DisjunctionSumScorer::scoring_doc_id() { return disi_->doc_id(); }

Result<float> DisjunctionSumScorer::get_max_score(int32_t upto) {
    float max_score = 0.0F;
    for (auto &scorer : sub_scorers_) {
        auto sub_max = scorer->get_max_score(upto);
        if (!sub_max.has_value()) {
            return std::unexpected(std::move(sub_max.error()));
        }
        max_score += sub_max.value();
    }
    return max_score;
}

TwoPhaseState DisjunctionSumScorer::has_two_phase_iterator() const {
    for (const auto &scorer : sub_scorers_) {
        if (scorer->has_two_phase_iterator() == TwoPhaseState::Yes) {
            return TwoPhaseState::Yes;
        }
    }
    return TwoPhaseState::No;
}

Result<bool> DisjunctionSumScorer::matches() {
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

float DisjunctionSumScorer::match_cost() const {
    float cost = 0.0F;
    for (const auto &scorer : sub_scorers_) {
        cost += scorer->match_cost();
    }
    return cost;
}

int32_t DisjunctionSumScorer::approx_doc_id() const { return disi_->doc_id(); }

Result<int32_t> DisjunctionSumScorer::approx_next_doc() { return disi_->next_doc(); }

Result<int32_t> DisjunctionSumScorer::approx_advance(int32_t target) { return disi_->advance(target); }

Result<int64_t> DisjunctionSumScorer::approx_cost() const { return disi_->cost(); }

Result<float> DisjunctionSumScorer::score() {
    const int32_t doc = disi_->doc_id();
    float score = 0.0F;
    for (auto &scorer : sub_scorers_) {
        if (scorer->doc_id() != doc) {
            continue;
        }
        auto sub_score = scorer->score();
        if (!sub_score.has_value()) {
            return std::unexpected(std::move(sub_score.error()));
        }
        score += sub_score.value();
    }
    return score;
}

Result<int32_t> DisjunctionSumScorer::freq() {
    const int32_t doc = disi_->doc_id();
    int32_t count = 0;
    for (const auto &scorer : sub_scorers_) {
        if (scorer->doc_id() == doc) {
            ++count;
        }
    }
    return count;
}

VoidResult DisjunctionSumScorer::set_min_competitive_score(float min_score) {
    for (auto &scorer : sub_scorers_) {
        if (auto set = scorer->set_min_competitive_score(min_score); !set.has_value()) {
            return set;
        }
    }
    return {};
}

} // namespace lucene
