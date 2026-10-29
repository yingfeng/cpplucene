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

module lucene_core:span_scorer.impl;

import :span_scorer;

import :error;
import :numeric_doc_values;
import :scorer;
import :similarity;
import :spans;
import :types;

import std;

namespace lucene {

SpanScorer::SpanScorer(std::unique_ptr<Spans> spans, std::shared_ptr<SimScorer> sim_scorer,
                       std::unique_ptr<NumericDocValues> norms)
    : spans_(std::move(spans)), sim_scorer_(std::move(sim_scorer)), norms_(std::move(norms)) {
    debug_check(spans_ != nullptr, "a span scorer needs spans");
}

Result<float> SpanScorer::get_max_score(int32_t /*upto*/) { return std::numeric_limits<float>::infinity(); }

Result<float> SpanScorer::score() {
    if (VoidResult ensured = ensure_freq(); !ensured.has_value()) {
        return std::unexpected(std::move(ensured.error()));
    }
    return score_current_doc();
}

Result<float> SpanScorer::sloppy_freq() {
    if (VoidResult ensured = ensure_freq(); !ensured.has_value()) {
        return std::unexpected(std::move(ensured.error()));
    }
    return freq_;
}

VoidResult SpanScorer::ensure_freq() {
    const int32_t current_doc = doc_id();
    if (last_scored_doc_ == current_doc) {
        return {};
    }
    if (VoidResult computed = set_freq_current_doc(); !computed.has_value()) {
        return computed;
    }
    last_scored_doc_ = current_doc;
    return {};
}

VoidResult SpanScorer::set_freq_current_doc() {
    freq_ = 0.0F;

    if (VoidResult started = spans_->do_start_current_doc(); !started.has_value()) {
        return started;
    }

    auto start = spans_->next_start_position();
    if (!start.has_value()) {
        return std::unexpected(std::move(start.error()));
    }
    if (start.value() == NO_MORE_POSITIONS) {
        // No positions at all: the document cannot match, and the score stays 0.
        return {};
    }

    while (true) {
        if (sim_scorer_ == nullptr) {
            // Scores are not needed; one match is enough.
            freq_ = 1.0F;
            return {};
        }
        freq_ += 1.0F / (1.0F + static_cast<float>(spans_->width()));
        if (VoidResult current = spans_->do_current_spans(); !current.has_value()) {
            return current;
        }
        auto next = spans_->next_start_position();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        if (next.value() == NO_MORE_POSITIONS) {
            return {};
        }
        start = next;
    }
}

Result<float> SpanScorer::score_current_doc() {
    if (sim_scorer_ == nullptr) {
        return err(LuceneError::illegal_state("a span scorer without a similarity cannot score"));
    }
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
    return sim_scorer_->score(freq_, norm);
}

} // namespace lucene
