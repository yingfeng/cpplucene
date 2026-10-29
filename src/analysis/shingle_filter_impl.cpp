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

module lucene_core:shingle_filter.impl;

import :shingle_filter;

import :attributes;
import :token_stream;
import :types;

import std;

namespace lucene {

ShingleFilter::ShingleFilter(std::unique_ptr<TokenStream> input, int32_t min_shingle_size, int32_t max_shingle_size,
                             bool output_unigrams)
    : TokenFilter(std::move(input)),
      min_shingle_size_(min_shingle_size),
      max_shingle_size_(max_shingle_size),
      output_unigrams_(output_unigrams) {
    if (min_shingle_size_ < 2) {
        min_shingle_size_ = 2;
    }
    if (max_shingle_size_ < min_shingle_size_) {
        max_shingle_size_ = min_shingle_size_;
    }
    attributes().add_attribute<CharTermAttribute>();
    attributes().add_attribute<PositionIncrementAttribute>();
}

VoidResult ShingleFilter::reset() {
    output_.clear();
    loaded_ = false;
    return TokenFilter::reset();
}

VoidResult ShingleFilter::build_output() {
    auto *term = attributes().get_attribute<CharTermAttribute>();
    auto *position_increment = attributes().get_attribute<PositionIncrementAttribute>();

    std::vector<PendingToken> tokens;
    std::vector<size_t> group_end; // one entry per token: the last token of its group
    while (true) {
        auto more = input_->increment_token();
        if (!more.has_value()) {
            return std::unexpected(std::move(more.error()));
        }
        if (!more.value()) {
            break;
        }
        PendingToken token;
        token.term = std::string(term->text());
        token.position_increment = position_increment->position_increment();
        tokens.push_back(std::move(token));
    }

    // A position increment greater than one is a gap: a shingle never spans one.
    group_end.resize(tokens.size());
    for (size_t start = 0; start < tokens.size();) {
        size_t stop = start + 1;
        while (stop < tokens.size() && tokens[stop].position_increment <= 1) {
            ++stop;
        }
        for (size_t index = start; index < stop; ++index) {
            group_end[index] = stop - 1;
        }
        start = stop;
    }

    output_.clear();
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (output_unigrams_) {
            output_.push_back(tokens[i]);
        }
        for (int32_t size = min_shingle_size_; size <= max_shingle_size_; ++size) {
            if (i + static_cast<size_t>(size) > tokens.size()) {
                break;
            }
            if (i + static_cast<size_t>(size) - 1 > group_end[i]) {
                // This shingle would span a gap.
                break;
            }
            PendingToken shingle;
            shingle.position_increment = 0;
            for (size_t j = i; j < i + static_cast<size_t>(size); ++j) {
                if (j > i) {
                    shingle.term += separator_;
                }
                shingle.term += tokens[j].term;
            }
            output_.push_back(std::move(shingle));
        }
    }
    return {};
}

Result<bool> ShingleFilter::increment_token() {
    if (!loaded_) {
        if (VoidResult built = build_output(); !built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        loaded_ = true;
    }
    if (output_.empty()) {
        return false;
    }

    PendingToken next = std::move(output_.front());
    output_.pop_front();

    auto *term = attributes().get_attribute<CharTermAttribute>();
    auto *position_increment = attributes().get_attribute<PositionIncrementAttribute>();
    term->set_text(next.term);
    position_increment->set_position_increment(next.position_increment);
    return true;
}

} // namespace lucene
