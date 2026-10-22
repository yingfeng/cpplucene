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

module lucene_core:term_scorer.impl;

import :term_scorer;

import :doc_id_set_iterator;
import :error;
import :numeric_doc_values;
import :postings_enum;
import :scorable;
import :scorer;
import :similarity;
import :types;

import std;

namespace lucene {

TermScorer::TermScorer(std::unique_ptr<PostingsEnum> postings, std::shared_ptr<SimScorer> sim_scorer,
                       std::unique_ptr<NumericDocValues> norms)
        : postings_(std::move(postings)), sim_scorer_(std::move(sim_scorer)), norms_(std::move(norms)) {}

int32_t TermScorer::doc_id() const {
    return postings_->doc_id();
}

Result<int32_t> TermScorer::next_doc() {
    return postings_->next_doc();
}

Result<int32_t> TermScorer::advance(int32_t target) {
    return postings_->advance(target);
}

Result<int32_t> TermScorer::slow_advance(int32_t target) {
    return postings_->slow_advance(target);
}

Result<int64_t> TermScorer::cost() const {
    return postings_->cost();
}

Result<int32_t> TermScorer::freq() {
    return postings_->freq();
}

Result<float> TermScorer::get_max_score(int32_t /*upto*/) {
    return std::numeric_limits<float>::infinity();
}

Result<std::pair<int32_t, int64_t>> TermScorer::freq_and_norm() {
    int64_t norm = 1;
    if (norms_ != nullptr) {
        auto exact = norms_->advance_exact(doc_id());
        if (!exact.has_value()) {
            return std::unexpected(std::move(exact.error()));
        }
        if (exact.value()) {
            auto value = norms_->long_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            norm = value.value();
        }
    }
    auto frequency = postings_->freq();
    if (!frequency.has_value()) {
        return std::unexpected(std::move(frequency.error()));
    }
    return std::pair<int32_t, int64_t>{frequency.value(), norm};
}

Result<float> TermScorer::score() {
    auto pair = freq_and_norm();
    if (!pair.has_value()) {
        return std::unexpected(std::move(pair.error()));
    }
    return sim_scorer_->score(static_cast<float>(pair.value().first), pair.value().second);
}

Result<float> TermScorer::smoothing_score(int32_t doc) {
    int64_t norm = 1;
    if (norms_ != nullptr) {
        auto exact = norms_->advance_exact(doc);
        if (!exact.has_value()) {
            return std::unexpected(std::move(exact.error()));
        }
        if (exact.value()) {
            auto value = norms_->long_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            norm = value.value();
        }
    }
    return sim_scorer_->score(0.0F, norm);
}

} // namespace lucene
