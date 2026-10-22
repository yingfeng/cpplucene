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

module lucene_core:req_excl_scorer.impl;

import :req_excl_scorer;

import :doc_id_set_iterator;
import :error;
import :scorer;
import :types;

import std;

namespace lucene {

namespace {

/// The estimated cost of one `advance` call, used to weigh the excluded clause's
/// confirmation (the reference implementation's `ADVANCE_COST`).
constexpr int32_t kAdvanceCost = 10;

/// Estimated number of operations needed to confirm a candidate; drives the
/// order of the two `matches` calls.
Result<float> estimate_match_cost(Scorer &required, Scorer &excluded) {
    float match_cost = 2.0F;
    if (required.has_two_phase_iterator() == TwoPhaseState::Yes) {
        // The required two-phase iterator always has to be confirmed.
        match_cost += required.match_cost();
    }
    const float excluded_match_cost = static_cast<float>(kAdvanceCost) + excluded.match_cost();

    auto required_cost = required.approx_cost();
    if (!required_cost.has_value()) {
        return std::unexpected(std::move(required_cost.error()));
    }
    auto excluded_cost = excluded.approx_cost();
    if (!excluded_cost.has_value()) {
        return std::unexpected(std::move(excluded_cost.error()));
    }

    // Upper bound for the ratio of documents the required approximation matches
    // that the excluded approximation matches as well.
    float ratio;
    if (required_cost.value() <= 0) {
        ratio = 1.0F;
    } else if (excluded_cost.value() <= 0) {
        ratio = 0.0F;
    } else {
        ratio = static_cast<float>(std::min(required_cost.value(), excluded_cost.value())) /
                static_cast<float>(required_cost.value());
    }
    match_cost += ratio * excluded_match_cost;
    return match_cost;
}

} // namespace

Result<std::unique_ptr<ReqExclScorer>> ReqExclScorer::create(std::unique_ptr<Scorer> required,
                                                            std::unique_ptr<Scorer> excluded) {
    if (required == nullptr || excluded == nullptr) {
        return err(LuceneError::illegal_argument("ReqExclScorer needs both a required and an excluded scorer"));
    }
    auto match_cost = estimate_match_cost(*required, *excluded);
    if (!match_cost.has_value()) {
        return std::unexpected(std::move(match_cost.error()));
    }
    bool check_req = true;
    if (required->has_two_phase_iterator() == TwoPhaseState::Yes) {
        check_req = excluded->has_two_phase_iterator() == TwoPhaseState::Yes &&
                    required->match_cost() <= excluded->match_cost();
    }
    return std::unique_ptr<ReqExclScorer>(
            new ReqExclScorer(std::move(required), std::move(excluded), match_cost.value(), check_req));
}

int32_t ReqExclScorer::doc_id() const { return required_->approx_doc_id(); }

Result<int32_t> ReqExclScorer::next_doc() { return required_->approx_next_doc(); }

Result<int32_t> ReqExclScorer::advance(int32_t target) { return required_->approx_advance(target); }

Result<int64_t> ReqExclScorer::cost() const { return required_->cost(); }

Result<int32_t> ReqExclScorer::scoring_doc_id() { return required_->scoring_doc_id(); }

Result<float> ReqExclScorer::get_max_score(int32_t upto) { return required_->get_max_score(upto); }

TwoPhaseState ReqExclScorer::has_two_phase_iterator() const { return TwoPhaseState::Yes; }

Result<bool> ReqExclScorer::matches() {
    const int32_t doc = required_->approx_doc_id();

    // Is the candidate excluded at all?
    int32_t excluded_doc = excluded_->approx_doc_id();
    if (excluded_doc < doc) {
        auto advanced = excluded_->approx_advance(doc);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        excluded_doc = advanced.value();
    }
    if (excluded_doc != doc) {
        // Not excluded: only the required confirmation is left.
        return required_->matches();
    }

    if (check_req_) {
        auto required_match = required_->matches();
        if (!required_match.has_value()) {
            return required_match;
        }
        if (!required_match.value()) {
            return false;
        }
        auto excluded_match = excluded_->matches();
        if (!excluded_match.has_value()) {
            return excluded_match;
        }
        return !excluded_match.value();
    }

    auto excluded_match = excluded_->matches();
    if (!excluded_match.has_value()) {
        return excluded_match;
    }
    if (excluded_match.value()) {
        return false;
    }
    return required_->matches();
}

float ReqExclScorer::match_cost() const { return match_cost_; }

int32_t ReqExclScorer::approx_doc_id() const { return required_->approx_doc_id(); }

Result<int32_t> ReqExclScorer::approx_next_doc() { return required_->approx_next_doc(); }

Result<int32_t> ReqExclScorer::approx_advance(int32_t target) { return required_->approx_advance(target); }

Result<int64_t> ReqExclScorer::approx_cost() const { return required_->approx_cost(); }

Result<float> ReqExclScorer::score() { return required_->score(); }

VoidResult ReqExclScorer::set_min_competitive_score(float min_score) {
    return required_->set_min_competitive_score(min_score);
}

} // namespace lucene
