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
import :numeric_doc_values;
import :phrase_matcher;
import :scorer;
import :similarity;
import :types;

namespace lucene {

PhraseScorer::PhraseScorer(std::unique_ptr<PhraseMatcher> matcher, std::shared_ptr<const SimScorer> sim_scorer,
                           std::unique_ptr<NumericDocValues> norms)
        : matcher_(std::move(matcher)), sim_scorer_(std::move(sim_scorer)), norms_(std::move(norms)) {}

Result<int32_t> PhraseScorer::do_next(int32_t target) {
    auto candidate = matcher_->advance(target);
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
        candidate = matcher_->next_doc();
    }
}

Result<int32_t> PhraseScorer::next_doc() {
    if (doc_ == NO_MORE_DOCS) {
        return doc_;
    }
    return do_next(doc_ + 1);
}

Result<int32_t> PhraseScorer::advance(int32_t target) {
    if (doc_ >= target) {
        return doc_;
    }
    return do_next(target);
}

Result<int64_t> PhraseScorer::cost() const {
    auto candidate_cost = matcher_->cost();
    if (!candidate_cost.has_value()) {
        return std::unexpected(std::move(candidate_cost.error()));
    }
    // Every candidate may have to be verified.
    return candidate_cost.value();
}

Result<float> PhraseScorer::get_max_score(int32_t /*upto*/) {
    return std::numeric_limits<float>::infinity();
}

float PhraseScorer::match_cost() const { return matcher_->get_match_cost(); }

Result<int32_t> PhraseScorer::approx_next_doc() { return matcher_->next_doc(); }

Result<int32_t> PhraseScorer::approx_advance(int32_t target) { return matcher_->advance(target); }

Result<int64_t> PhraseScorer::approx_cost() const { return matcher_->cost(); }

Result<bool> PhraseScorer::matches() {
    if (freq_doc_ == doc_) {
        return freq_ != 0.0F;
    }
    auto reset = matcher_->reset();
    if (!reset.has_value()) {
        return std::unexpected(std::move(reset.error()));
    }
    auto first = matcher_->next_match();
    if (!first.has_value()) {
        return std::unexpected(std::move(first.error()));
    }
    if (!first.value()) {
        freq_ = 0.0F;
        freq_doc_ = doc_;
        return false;
    }
    float freq = matcher_->sloppy_weight();
    while (true) {
        auto next = matcher_->next_match();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        if (!next.value()) {
            break;
        }
        freq += matcher_->sloppy_weight();
    }
    freq_ = freq;
    freq_doc_ = doc_;
    return true;
}

Result<float> PhraseScorer::score() {
    auto matched = matches();
    if (!matched.has_value()) {
        return std::unexpected(std::move(matched.error()));
    }
    if (!matched.value()) {
        return 0.0F;
    }
    int64_t norm = 1;
    if (norms_ != nullptr) {
        auto advanced = norms_->advance_exact(doc_);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (!advanced.value()) {
            return err(LuceneError::illegal_state("the document has no norm"));
        }
        auto value = norms_->long_value();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        norm = value.value();
    }
    return sim_scorer_->score(freq_, norm);
}

} // namespace lucene
