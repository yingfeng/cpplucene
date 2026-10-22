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

module lucene_core:conjunction_scorer.impl;

import :conjunction_scorer;

import :conjunction_disi;
import :doc_id_set_iterator;
import :error;
import :scorer;
import :types;

import std;

namespace lucene {

ConjunctionScorer::ConjunctionScorer(std::vector<std::unique_ptr<Scorer>> required, std::vector<size_t> scoring)
    : required_(std::move(required)), scoring_(std::move(scoring)) {}

Result<std::unique_ptr<ConjunctionScorer>> ConjunctionScorer::create(std::vector<std::unique_ptr<Scorer>> required,
                                                                    std::vector<size_t> scoring) {
    if (required.size() < 2) {
        return err(LuceneError::illegal_argument("There must be at least 2 sub-scorers, got " +
                                                 std::to_string(required.size())));
    }
    std::vector<DocIdSetIterator *> iterators;
    iterators.reserve(required.size());
    for (const auto &scorer : required) {
        if (scorer == nullptr) {
            return err(LuceneError::illegal_argument("ConjunctionScorer sub-scorers must not be null"));
        }
        iterators.push_back(scorer.get());
    }
    for (const size_t index : scoring) {
        if (index >= required.size()) {
            return err(LuceneError::illegal_argument("scoring index out of range: " + std::to_string(index)));
        }
    }

    auto conjunction = ConjunctionDISI::create(iterators);
    if (!conjunction.has_value()) {
        return std::unexpected(std::move(conjunction.error()));
    }

    std::unique_ptr<ConjunctionScorer> scorer(new ConjunctionScorer(std::move(required), std::move(scoring)));
    scorer->disi_ = std::move(conjunction.value());
    return scorer;
}

int32_t ConjunctionScorer::doc_id() const { return disi_->doc_id(); }

Result<int32_t> ConjunctionScorer::next_doc() { return disi_->next_doc(); }

Result<int32_t> ConjunctionScorer::advance(int32_t target) { return disi_->advance(target); }

Result<int64_t> ConjunctionScorer::cost() const { return disi_->cost(); }

Result<int32_t> ConjunctionScorer::scoring_doc_id() { return disi_->doc_id(); }

Result<float> ConjunctionScorer::get_max_score(int32_t upto) {
    // Every sub-scorer contributes, including the ones that do not score: their
    // upper bound is still a bound for the sum of the scoring ones.
    float max_score = 0.0F;
    for (auto &scorer : required_) {
        auto sub_max = scorer->get_max_score(upto);
        if (!sub_max.has_value()) {
            return std::unexpected(std::move(sub_max.error()));
        }
        max_score += sub_max.value();
    }
    return max_score;
}

TwoPhaseState ConjunctionScorer::has_two_phase_iterator() const {
    for (const auto &scorer : required_) {
        if (scorer->has_two_phase_iterator() == TwoPhaseState::Yes) {
            return TwoPhaseState::Yes;
        }
    }
    return TwoPhaseState::No;
}

Result<bool> ConjunctionScorer::matches() {
    for (auto &scorer : required_) {
        if (scorer->has_two_phase_iterator() == TwoPhaseState::Yes) {
            auto matched = scorer->matches();
            if (!matched.has_value()) {
                return matched;
            }
            if (!matched.value()) {
                return false;
            }
        }
    }
    return true;
}

float ConjunctionScorer::match_cost() const {
    float cost = 0.0F;
    for (const auto &scorer : required_) {
        cost += scorer->match_cost();
    }
    return cost;
}

int32_t ConjunctionScorer::approx_doc_id() const { return disi_->doc_id(); }

Result<int32_t> ConjunctionScorer::approx_next_doc() { return disi_->next_doc(); }

Result<int32_t> ConjunctionScorer::approx_advance(int32_t target) { return disi_->advance(target); }

Result<int64_t> ConjunctionScorer::approx_cost() const { return disi_->cost(); }

Result<float> ConjunctionScorer::score() {
    float score = 0.0F;
    for (const size_t index : scoring_) {
        auto sub_score = required_[index]->score();
        if (!sub_score.has_value()) {
            return std::unexpected(std::move(sub_score.error()));
        }
        score += sub_score.value();
    }
    return score;
}

VoidResult ConjunctionScorer::set_min_competitive_score(float min_score) {
    for (auto &scorer : required_) {
        if (auto set = scorer->set_min_competitive_score(min_score); !set.has_value()) {
            return set;
        }
    }
    return {};
}

} // namespace lucene
