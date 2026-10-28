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

module lucene_core:minimum_should_match_scorer.impl;

import :minimum_should_match_scorer;

import :disjunction_disi_approximation;
import :doc_id_set_iterator;
import :error;
import :scorer;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<MinimumShouldMatchScorer>> MinimumShouldMatchScorer::create(
        std::vector<std::unique_ptr<Scorer>> sub_scorers, int32_t min_should_match) {
    if (sub_scorers.size() < 2) {
        return err(LuceneError::illegal_argument("There must be at least 2 sub-scorers, got " +
                                                 std::to_string(sub_scorers.size())));
    }
    if (min_should_match < 1 || static_cast<size_t>(min_should_match) > sub_scorers.size()) {
        return err(LuceneError::illegal_argument("minimum_should_match must be in [1, " +
                                                 std::to_string(sub_scorers.size()) + "], got " +
                                                 std::to_string(min_should_match)));
    }
    std::vector<Scorer *> raw;
    raw.reserve(sub_scorers.size());
    for (const auto &scorer : sub_scorers) {
        if (scorer == nullptr) {
            return err(LuceneError::illegal_argument("MinimumShouldMatchScorer sub-scorers must not be null"));
        }
        raw.push_back(scorer.get());
    }
    auto approximation = DisjunctionDISIApproximation::create(raw);
    if (!approximation.has_value()) {
        return std::unexpected(std::move(approximation.error()));
    }
    return std::unique_ptr<MinimumShouldMatchScorer>(new MinimumShouldMatchScorer(
            std::move(sub_scorers), std::move(approximation.value()), min_should_match));
}

int32_t MinimumShouldMatchScorer::doc_id() const { return approximation_->doc_id(); }

Result<int32_t> MinimumShouldMatchScorer::next_doc() { return approximation_->next_doc(); }

Result<int32_t> MinimumShouldMatchScorer::advance(int32_t target) { return approximation_->advance(target); }

Result<int64_t> MinimumShouldMatchScorer::cost() const { return approximation_->cost(); }

Result<int32_t> MinimumShouldMatchScorer::scoring_doc_id() { return approximation_->doc_id(); }

Result<float> MinimumShouldMatchScorer::get_max_score(int32_t upto) {
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

Result<bool> MinimumShouldMatchScorer::matches() {
    const int32_t doc = approximation_->doc_id();
    int32_t matching = 0;
    for (auto &scorer : sub_scorers_) {
        // The approximation only moves its head, so a sub-scorer may still be
        // behind the current document.  Bring it forward before counting, which
        // is what the reference implementation's `DisjunctionScorer` does.
        if (scorer->doc_id() < doc) {
            auto advanced = scorer->advance(doc);
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
        }
        if (scorer->doc_id() != doc) {
            continue;
        }
        if (scorer->has_two_phase_iterator() == TwoPhaseState::Yes) {
            auto confirmed = scorer->matches();
            if (!confirmed.has_value()) {
                return confirmed;
            }
            if (!confirmed.value()) {
                continue;
            }
        }
        ++matching;
        if (matching >= min_should_match_) {
            return true;
        }
    }
    return false;
}

float MinimumShouldMatchScorer::match_cost() const {
    float cost = 0.0F;
    for (const auto &scorer : sub_scorers_) {
        cost += scorer->match_cost();
    }
    return cost;
}

int32_t MinimumShouldMatchScorer::approx_doc_id() const { return approximation_->doc_id(); }

Result<int32_t> MinimumShouldMatchScorer::approx_next_doc() { return approximation_->next_doc(); }

Result<int32_t> MinimumShouldMatchScorer::approx_advance(int32_t target) { return approximation_->advance(target); }

Result<int64_t> MinimumShouldMatchScorer::approx_cost() const { return approximation_->cost(); }

Result<float> MinimumShouldMatchScorer::score() {
    const int32_t doc = approximation_->doc_id();
    float score = 0.0F;
    for (auto &scorer : sub_scorers_) {
        if (scorer->doc_id() != doc) {
            continue;
        }
        if (scorer->has_two_phase_iterator() == TwoPhaseState::Yes) {
            auto confirmed = scorer->matches();
            if (!confirmed.has_value()) {
                return std::unexpected(std::move(confirmed.error()));
            }
            if (!confirmed.value()) {
                continue;
            }
        }
        auto sub_score = scorer->score();
        if (!sub_score.has_value()) {
            return std::unexpected(std::move(sub_score.error()));
        }
        score += sub_score.value();
    }
    return score;
}

VoidResult MinimumShouldMatchScorer::set_min_competitive_score(float min_score) {
    for (auto &scorer : sub_scorers_) {
        if (auto set = scorer->set_min_competitive_score(min_score); !set.has_value()) {
            return set;
        }
    }
    return {};
}

} // namespace lucene
