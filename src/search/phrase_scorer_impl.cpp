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

module lucene_core:phrase_scorer.impl;

import :phrase_scorer;

import std;

import :doc_id_set_iterator;
import :error;
import :impacts_disi;
import :max_score_cache;
import :numeric_doc_values;
import :phrase_matcher;
import :score_mode;
import :scorer;
import :similarity;
import :types;

namespace lucene {

namespace {

/// The matcher's candidate cursor as a [`DocIdSetIterator`], so that an
/// [`ImpactsDISI`] can drive it and skip blocks the impacts rule out.
class MatcherApproximation final : public DocIdSetIterator {
public:
    explicit MatcherApproximation(PhraseMatcher &matcher) : matcher_(matcher) {}

    [[nodiscard]] int32_t doc_id() const override { return matcher_.doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return matcher_.next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return matcher_.advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return matcher_.cost(); }

private:
    PhraseMatcher &matcher_;
};

} // namespace

PhraseScorer::PhraseScorer(std::unique_ptr<PhraseMatcher> matcher, std::shared_ptr<const SimScorer> sim_scorer,
                           std::unique_ptr<NumericDocValues> norms, ScoreMode score_mode)
        : matcher_(std::move(matcher)), sim_scorer_(std::move(sim_scorer)), norms_(std::move(norms)),
          score_mode_(score_mode) {
    exact_ = dynamic_cast<ExactPhraseMatcher *>(matcher_.get());
    if (score_mode_ != ScoreMode::TopScores || sim_scorer_ == nullptr || matcher_->impacts() == nullptr) {
        // Without impacts (or without a scoring need) the matcher is walked as
        // is, which is always correct and never skips anything.
        return;
    }
    approximation_ = std::make_unique<MatcherApproximation>(*matcher_);
    max_score_cache_ = std::make_unique<MaxScoreCache>(matcher_->impacts(), sim_scorer_);
    impacts_approximation_ = std::make_unique<ImpactsDISI>(approximation_.get(), max_score_cache_.get());
}

Result<int32_t> PhraseScorer::next_candidate() {
    if (impacts_approximation_ != nullptr) {
        return impacts_approximation_->next_doc();
    }
    return matcher_->next_doc();
}

Result<int32_t> PhraseScorer::advance_candidate(int32_t target) {
    if (impacts_approximation_ != nullptr) {
        return impacts_approximation_->advance(target);
    }
    return matcher_->advance(target);
}

Result<int32_t> PhraseScorer::do_next(int32_t target, bool advance) {
    auto candidate = advance ? advance_candidate(target) : next_candidate();
    while (true) {
        if (!candidate.has_value()) {
            return std::unexpected(std::move(candidate.error()));
        }
        if (candidate.value() == NO_MORE_DOCS) {
            doc_ = NO_MORE_DOCS;
            return doc_;
        }
        doc_ = candidate.value();
        auto matched = matches();
        if (!matched.has_value()) {
            return std::unexpected(std::move(matched.error()));
        }
        if (matched.value()) {
            return doc_;
        }
        candidate = next_candidate();
    }
}

Result<int32_t> PhraseScorer::next_doc() {
    if (doc_ == NO_MORE_DOCS) {
        return doc_;
    }
    return do_next(doc_ + 1, false);
}

Result<int32_t> PhraseScorer::advance(int32_t target) {
    if (doc_ >= target) {
        return doc_;
    }
    return do_next(target, true);
}

Result<int64_t> PhraseScorer::cost() const {
    auto candidate_cost = matcher_->cost();
    if (!candidate_cost.has_value()) {
        return std::unexpected(std::move(candidate_cost.error()));
    }
    // Every candidate may have to be verified.
    return candidate_cost.value();
}

Result<int32_t> PhraseScorer::advance_shallow(int32_t target) {
    if (max_score_cache_ == nullptr) {
        return NO_MORE_DOCS;
    }
    return max_score_cache_->advance_shallow(target);
}

Result<float> PhraseScorer::get_max_score(int32_t upto) {
    if (max_score_cache_ == nullptr) {
        return std::numeric_limits<float>::infinity();
    }
    return max_score_cache_->get_max_score(upto);
}

VoidResult PhraseScorer::set_min_competitive_score(float min_score) {
    if (min_score <= min_competitive_score_) {
        return {};
    }
    min_competitive_score_ = min_score;
    if (impacts_approximation_ != nullptr) {
        impacts_approximation_->set_min_competitive_score(min_competitive_score_);
    }
    return {};
}

float PhraseScorer::match_cost() const { return matcher_->get_match_cost(); }

Result<int32_t> PhraseScorer::approx_next_doc() { return matcher_->next_doc(); }

Result<int32_t> PhraseScorer::approx_advance(int32_t target) { return matcher_->advance(target); }

Result<int64_t> PhraseScorer::approx_cost() const { return matcher_->cost(); }

Result<int64_t> PhraseScorer::norm_of_current_doc() {
    if (norms_ == nullptr) {
        return int64_t{1};
    }
    auto advanced = norms_->advance_exact(doc_);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    if (!advanced.value()) {
        return int64_t{1};
    }
    auto value = norms_->long_value();
    if (!value.has_value()) {
        return std::unexpected(std::move(value.error()));
    }
    return value.value();
}

Result<bool> PhraseScorer::matches() {
    if (freq_doc_ == doc_) {
        return freq_ != 0.0F;
    }
    if (score_mode_ == ScoreMode::TopScores && min_competitive_score_ > 0.0F) {
        // Even the largest phrase frequency of this document may not be enough
        // to compete: bail out before decoding any position.
        auto max_freq = max_freq_of_matcher();
        if (!max_freq.has_value()) {
            return std::unexpected(std::move(max_freq.error()));
        }
        auto norm = norm_of_current_doc();
        if (!norm.has_value()) {
            return std::unexpected(std::move(norm.error()));
        }
        if (sim_scorer_->score(max_freq.value(), norm.value()) < min_competitive_score_) {
            freq_ = 0.0F;
            freq_doc_ = doc_;
            freq_complete_ = true;
            return false;
        }
    }
    auto reset = reset_matcher();
    if (!reset.has_value()) {
        return std::unexpected(std::move(reset.error()));
    }
    // Only the first match: a count never needs the frequency, and enumerating
    // the rest of the positions of the document would be pure waste there.
    auto first = next_match_of_matcher();
    if (!first.has_value()) {
        return std::unexpected(std::move(first.error()));
    }
    freq_ = first.value() ? match_weight() : 0.0F;
    // A mismatch is a complete answer; a match only shows that the frequency is
    // at least this much.
    freq_complete_ = !first.value();
    freq_doc_ = doc_;
    return first.value();
}

Result<float> PhraseScorer::score() {
    auto matched = matches();
    if (!matched.has_value()) {
        return std::unexpected(std::move(matched.error()));
    }
    if (!matched.value()) {
        return 0.0F;
    }
    if (!freq_complete_) {
        // Enumerate the matches the confirmation left unread, now that the
        // frequency is really needed.
        float freq = freq_;
        while (true) {
            auto next = next_match_of_matcher();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            if (!next.value()) {
                break;
            }
            freq += match_weight();
        }
        freq_ = freq;
        freq_complete_ = true;
    }
    auto norm = norm_of_current_doc();
    if (!norm.has_value()) {
        return std::unexpected(std::move(norm.error()));
    }
    return sim_scorer_->score(freq_, norm.value());
}

} // namespace lucene
