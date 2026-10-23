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

module lucene_core:doc_values_range_iterator.impl;

import :doc_values_range_iterator;

import std;

import :doc_id_set_iterator;
import :doc_values_skipper;
import :error;
import :two_phase_iterator;
import :types;

namespace lucene {

DocValuesRangeIterator::DocValuesRangeIterator(std::unique_ptr<TwoPhaseIterator> inner,
                                               std::shared_ptr<DocValuesSkipper> skipper, int64_t lower_value,
                                               int64_t upper_value, bool has_gaps)
        : inner_(std::move(inner)),
          skipper_(std::move(skipper)),
          lower_value_(lower_value),
          upper_value_(upper_value),
          has_gaps_(has_gaps) {}

Result<Match> DocValuesRangeIterator::match_at_level(size_t level) const {
    const int64_t min_value = skipper_->min_value(level);
    const int64_t max_value = skipper_->max_value(level);

    if (min_value > upper_value_ || max_value < lower_value_) {
        return Match::kNo;
    }
    if (has_gaps_) {
        // Documents without a value may sit inside the interval, so a matching
        // value range is not enough to accept the whole interval.
        return Match::kMaybe;
    }
    if (min_value >= lower_value_ && max_value <= upper_value_) {
        const int32_t doc_count = skipper_->doc_count(level);
        const int32_t max_doc_id = skipper_->max_doc_id(level);
        const int32_t min_doc_id = skipper_->min_doc_id(level);
        if (doc_count == max_doc_id - min_doc_id + 1) {
            return Match::kYes;
        }
        return Match::kIfDocHasValue;
    }
    return Match::kMaybe;
}

Result<int32_t> DocValuesRangeIterator::approx_advance(int32_t target) {
    for (;;) {
        if (target > upto_) {
            auto advanced = skipper_->advance(target);
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
            // The skipper may stop on the interval that follows `target` when
            // `target` itself has no value.
            target = std::max(target, skipper_->min_doc_id(0));
            if (target == NO_MORE_DOCS) {
                doc_ = NO_MORE_DOCS;
                return doc_;
            }
            upto_ = skipper_->max_doc_id(0);
            auto decided = match_at_level(0);
            if (!decided.has_value()) {
                return std::unexpected(std::move(decided.error()));
            }
            match_ = decided.value();

            // A decision on level 0 may also hold for a wider range.
            size_t next_level = 1;
            while (match_ != Match::kMaybe && next_level < skipper_->num_levels()) {
                auto wider = match_at_level(next_level);
                if (!wider.has_value()) {
                    return std::unexpected(std::move(wider.error()));
                }
                if (wider.value() != match_) {
                    break;
                }
                upto_ = skipper_->max_doc_id(next_level);
                ++next_level;
            }
        }

        switch (match_) {
            case Match::kYes:
                doc_ = target;
                return doc_;
            case Match::kMaybe:
            case Match::kIfDocHasValue: {
                if (target > inner_->approx_doc_id()) {
                    auto next = inner_->approx_advance(target);
                    if (!next.has_value()) {
                        return std::unexpected(std::move(next.error()));
                    }
                    target = next.value();
                }
                if (target <= upto_) {
                    doc_ = target;
                    return doc_;
                }
                // The candidate left the interval, so the loop advances the
                // skipper again.
                break;
            }
            case Match::kNo: {
                if (upto_ == NO_MORE_DOCS) {
                    doc_ = NO_MORE_DOCS;
                    return doc_;
                }
                target = upto_ + 1;
                break;
            }
        }
    }
}

Result<int64_t> DocValuesRangeIterator::approx_cost() const { return inner_->approx_cost(); }

Result<bool> DocValuesRangeIterator::matches() {
    switch (match_) {
        case Match::kYes:
        case Match::kIfDocHasValue:
            // A document of the interval has a value only because the interval is
            // contiguous there, so it matches.
            return true;
        case Match::kMaybe:
            return inner_->matches();
        case Match::kNo:
            return err(LuceneError::illegal_state("the approximation is not positioned"));
    }
    return err(LuceneError::illegal_state("unknown match state"));
}

} // namespace lucene
