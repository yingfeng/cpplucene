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

module lucene_core:ngram_token_filter.impl;

import :ngram_token_filter;

import :attributes;
import :string_helper;
import :token_stream;
import :types;

import std;

namespace lucene {

namespace {

/// Byte offset of every code point of `text`, plus the length at the end.
std::vector<size_t> code_point_offsets(std::string_view text) {
    std::vector<size_t> offsets;
    offsets.reserve(text.size() + 1);
    size_t position = 0;
    while (position < text.size()) {
        offsets.push_back(position);
        size_t bytes = 1;
        auto code_point = StringHelper::utf8_code_point_at(text, position, bytes);
        if (!code_point.has_value() || bytes == 0) {
            bytes = 1;
        }
        position += bytes;
    }
    offsets.push_back(text.size());
    return offsets;
}

} // namespace

NGramTokenFilter::NGramTokenFilter(std::unique_ptr<TokenStream> input, int32_t min_gram, int32_t max_gram,
                                   bool preserve_original)
    : TokenFilter(std::move(input)), min_gram_(min_gram), max_gram_(max_gram), preserve_original_(preserve_original) {
    if (min_gram_ < 1) {
        min_gram_ = 1;
    }
    if (max_gram_ < min_gram_) {
        max_gram_ = min_gram_;
    }
    gram_size_ = min_gram_;
    attributes().add_attribute<CharTermAttribute>();
    attributes().add_attribute<PositionIncrementAttribute>();
}

NGramTokenFilter::NGramTokenFilter(std::unique_ptr<TokenStream> input, int32_t gram_size)
    : NGramTokenFilter(std::move(input), gram_size, gram_size, false) {}

VoidResult NGramTokenFilter::reset() {
    current_.clear();
    offsets_.clear();
    code_point_count_ = 0;
    gram_start_ = 0;
    gram_size_ = 0;
    position_increment_ = 0;
    active_ = false;
    return TokenFilter::reset();
}

Result<bool> NGramTokenFilter::increment_token() {
    auto *term = attributes().get_attribute<CharTermAttribute>();
    auto *position_increment = attributes().get_attribute<PositionIncrementAttribute>();

    while (true) {
        if (!active_) {
            auto more = input_->increment_token();
            if (!more.has_value()) {
                return std::unexpected(std::move(more.error()));
            }
            if (!more.value()) {
                return false;
            }

            current_ = std::string(term->text());
            offsets_ = code_point_offsets(current_);
            code_point_count_ = static_cast<int32_t>(offsets_.size()) - 1;
            position_increment_ += position_increment->position_increment();
            gram_start_ = 0;
            gram_size_ = min_gram_;

            if (preserve_original_ && code_point_count_ < min_gram_) {
                // Shorter than minGram, but the original is kept as its own term.
                position_increment->set_position_increment(position_increment_);
                position_increment_ = 0;
                active_ = false;
                return true;
            }
            active_ = true;
        }

        if (gram_size_ > max_gram_ || gram_start_ + gram_size_ > code_point_count_) {
            ++gram_start_;
            gram_size_ = min_gram_;
        }
        if (gram_start_ + gram_size_ <= code_point_count_) {
            const size_t start = offsets_[static_cast<size_t>(gram_start_)];
            const size_t end = offsets_[static_cast<size_t>(gram_start_ + gram_size_)];
            term->set_text(current_.substr(start, end - start));
            position_increment->set_position_increment(position_increment_);
            position_increment_ = 0;
            ++gram_size_;
            return true;
        }
        if (preserve_original_ && code_point_count_ > max_gram_) {
            // Longer than maxGram: keep the original as well.
            term->set_text(current_);
            position_increment->set_position_increment(0);
            active_ = false;
            return true;
        }
        active_ = false;
    }
}

EdgeNGramTokenFilter::EdgeNGramTokenFilter(std::unique_ptr<TokenStream> input, int32_t min_gram, int32_t max_gram,
                                           bool preserve_original)
    : TokenFilter(std::move(input)), min_gram_(min_gram), max_gram_(max_gram), preserve_original_(preserve_original) {
    if (min_gram_ < 1) {
        min_gram_ = 1;
    }
    if (max_gram_ < min_gram_) {
        max_gram_ = min_gram_;
    }
    gram_size_ = min_gram_;
    attributes().add_attribute<CharTermAttribute>();
    attributes().add_attribute<PositionIncrementAttribute>();
}

VoidResult EdgeNGramTokenFilter::reset() {
    current_.clear();
    offsets_.clear();
    code_point_count_ = 0;
    gram_size_ = 0;
    position_increment_ = 0;
    active_ = false;
    return TokenFilter::reset();
}

Result<bool> EdgeNGramTokenFilter::increment_token() {
    auto *term = attributes().get_attribute<CharTermAttribute>();
    auto *position_increment = attributes().get_attribute<PositionIncrementAttribute>();

    while (true) {
        if (!active_) {
            auto more = input_->increment_token();
            if (!more.has_value()) {
                return std::unexpected(std::move(more.error()));
            }
            if (!more.value()) {
                return false;
            }

            current_ = std::string(term->text());
            offsets_ = code_point_offsets(current_);
            code_point_count_ = static_cast<int32_t>(offsets_.size()) - 1;
            position_increment_ += position_increment->position_increment();
            gram_size_ = min_gram_;

            if (preserve_original_ && code_point_count_ < min_gram_) {
                position_increment->set_position_increment(position_increment_);
                position_increment_ = 0;
                active_ = false;
                return true;
            }
            active_ = true;
        }

        if (gram_size_ <= max_gram_ && gram_size_ <= code_point_count_) {
            const size_t end = offsets_[static_cast<size_t>(gram_size_)];
            term->set_text(current_.substr(0, end));
            position_increment->set_position_increment(position_increment_);
            position_increment_ = 0;
            ++gram_size_;
            return true;
        }
        if (preserve_original_ && code_point_count_ > max_gram_) {
            term->set_text(current_);
            position_increment->set_position_increment(0);
            active_ = false;
            return true;
        }
        active_ = false;
    }
}

} // namespace lucene
