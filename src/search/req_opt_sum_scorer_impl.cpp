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

module lucene_core:req_opt_sum_scorer.impl;

import :req_opt_sum_scorer;

import :doc_id_set_iterator;
import :error;
import :scorer;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<ReqOptSumScorer>> ReqOptSumScorer::create(std::unique_ptr<Scorer> required,
                                                                std::unique_ptr<Scorer> optional) {
    if (required == nullptr || optional == nullptr) {
        return err(LuceneError::illegal_argument("ReqOptSumScorer needs both a required and an optional scorer"));
    }
    return std::unique_ptr<ReqOptSumScorer>(new ReqOptSumScorer(std::move(required), std::move(optional)));
}

int32_t ReqOptSumScorer::doc_id() const { return required_->doc_id(); }

Result<int32_t> ReqOptSumScorer::next_doc() { return required_->next_doc(); }

Result<int32_t> ReqOptSumScorer::advance(int32_t target) { return required_->advance(target); }

Result<int64_t> ReqOptSumScorer::cost() const { return required_->cost(); }

Result<int32_t> ReqOptSumScorer::scoring_doc_id() { return required_->scoring_doc_id(); }

Result<float> ReqOptSumScorer::get_max_score(int32_t upto) {
    auto required_max = required_->get_max_score(upto);
    if (!required_max.has_value()) {
        return std::unexpected(std::move(required_max.error()));
    }
    float max_score = required_max.value();

    auto optional_doc = optional_->scoring_doc_id();
    if (!optional_doc.has_value()) {
        return std::unexpected(std::move(optional_doc.error()));
    }
    if (optional_doc.value() <= upto) {
        auto optional_max = optional_->get_max_score(upto);
        if (!optional_max.has_value()) {
            return std::unexpected(std::move(optional_max.error()));
        }
        max_score += optional_max.value();
    }
    return max_score;
}

TwoPhaseState ReqOptSumScorer::has_two_phase_iterator() const {
    // Only the required clause decides the document set, so only its
    // confirmation matters.
    return required_->has_two_phase_iterator();
}

Result<bool> ReqOptSumScorer::matches() { return required_->matches(); }

float ReqOptSumScorer::match_cost() const { return required_->match_cost(); }

int32_t ReqOptSumScorer::approx_doc_id() const { return required_->approx_doc_id(); }

Result<int32_t> ReqOptSumScorer::approx_next_doc() { return required_->approx_next_doc(); }

Result<int32_t> ReqOptSumScorer::approx_advance(int32_t target) { return required_->approx_advance(target); }

Result<int64_t> ReqOptSumScorer::approx_cost() const { return required_->approx_cost(); }

Result<float> ReqOptSumScorer::score() {
    auto current = required_->scoring_doc_id();
    if (!current.has_value()) {
        return std::unexpected(std::move(current.error()));
    }
    const int32_t current_doc = current.value();

    auto required_score = required_->score();
    if (!required_score.has_value()) {
        return std::unexpected(std::move(required_score.error()));
    }
    float score = required_score.value();

    int32_t optional_doc = optional_->approx_doc_id();
    if (optional_doc < current_doc) {
        auto advanced = optional_->approx_advance(current_doc);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        optional_doc = advanced.value();
        if (optional_doc == current_doc) {
            auto matched = optional_->matches();
            if (!matched.has_value()) {
                return std::unexpected(std::move(matched.error()));
            }
            if (!matched.value()) {
                // The candidate is not a real match for the optional clause.
                auto next = optional_->approx_next_doc();
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
                optional_doc = next.value();
            }
        }
    }

    if (optional_doc == current_doc) {
        auto optional_score = optional_->score();
        if (!optional_score.has_value()) {
            return std::unexpected(std::move(optional_score.error()));
        }
        score += optional_score.value();
    }
    return score;
}

VoidResult ReqOptSumScorer::set_min_competitive_score(float min_score) {
    if (auto set = required_->set_min_competitive_score(min_score); !set.has_value()) {
        return set;
    }
    return optional_->set_min_competitive_score(min_score);
}

} // namespace lucene
