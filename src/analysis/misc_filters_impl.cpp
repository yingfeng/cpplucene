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

module lucene_core:misc_filters.impl;

import :misc_filters;

import :attributes;
import :error;
import :string_helper;
import :token_stream;
import :types;
import :unicode_util;

import std;

namespace lucene {

namespace {

/// The number of code points of `text`.
[[nodiscard]] size_t code_point_length(std::string_view text) {
    size_t count = 0;
    size_t position = 0;
    while (position < text.size()) {
        size_t bytes = 1;
        auto code_point = StringHelper::utf8_code_point_at(text, position, bytes);
        if (!code_point.has_value() || bytes == 0) {
            bytes = 1;
        }
        position += bytes;
        ++count;
    }
    return count;
}

/// The first `length` code points of `text`.
[[nodiscard]] std::string truncate_to(std::string_view text, size_t length) {
    size_t count = 0;
    size_t position = 0;
    while (position < text.size() && count < length) {
        size_t bytes = 1;
        auto code_point = StringHelper::utf8_code_point_at(text, position, bytes);
        if (!code_point.has_value() || bytes == 0) {
            bytes = 1;
        }
        position += bytes;
        ++count;
    }
    return std::string(text.substr(0, position));
}

} // namespace

LengthFilter::LengthFilter(std::unique_ptr<TokenStream> input, size_t min, size_t max)
    : TokenFilter(std::move(input)), min_(min), max_(max) {
    attributes().add_attribute<CharTermAttribute>();
    attributes().add_attribute<PositionIncrementAttribute>();
}

VoidResult LengthFilter::reset() {
    skipped_positions_ = 0;
    return TokenFilter::reset();
}

Result<bool> LengthFilter::increment_token() {
    auto *term = attributes().get_attribute<CharTermAttribute>();
    auto *position_increment = attributes().get_attribute<PositionIncrementAttribute>();

    while (true) {
        auto more = input_->increment_token();
        if (!more.has_value()) {
            return std::unexpected(std::move(more.error()));
        }
        if (!more.value()) {
            return false;
        }
        const size_t length = code_point_length(term->text());
        if (length >= min_ && length <= max_) {
            if (skipped_positions_ != 0) {
                position_increment->set_position_increment(position_increment->position_increment() +
                                                           skipped_positions_);
                skipped_positions_ = 0;
            }
            return true;
        }
        skipped_positions_ += position_increment->position_increment();
    }
}

TruncateTokenFilter::TruncateTokenFilter(std::unique_ptr<TokenStream> input, size_t length)
    : TokenFilter(std::move(input)), length_(length) {
    attributes().add_attribute<CharTermAttribute>();
}

Result<bool> TruncateTokenFilter::increment_token() {
    auto more = input_->increment_token();
    if (!more.has_value()) {
        return std::unexpected(std::move(more.error()));
    }
    if (!more.value()) {
        return false;
    }
    auto *term = attributes().get_attribute<CharTermAttribute>();
    if (code_point_length(term->text()) > length_) {
        term->set_text(truncate_to(term->text(), length_));
    }
    return true;
}

LimitTokenCountFilter::LimitTokenCountFilter(std::unique_ptr<TokenStream> input, size_t max_token_count,
                                             bool consume_all_tokens)
    : TokenFilter(std::move(input)), max_token_count_(max_token_count), consume_all_tokens_(consume_all_tokens) {}

VoidResult LimitTokenCountFilter::reset() {
    token_count_ = 0;
    exhausted_ = false;
    return TokenFilter::reset();
}

Result<bool> LimitTokenCountFilter::increment_token() {
    if (exhausted_) {
        return false;
    }
    if (token_count_ < max_token_count_) {
        auto more = input_->increment_token();
        if (!more.has_value()) {
            return std::unexpected(std::move(more.error()));
        }
        if (more.value()) {
            ++token_count_;
        } else {
            exhausted_ = true;
        }
        return more.value();
    }

    // The limit is reached: either stop right here or drain the rest.
    if (!consume_all_tokens_) {
        exhausted_ = true;
        return false;
    }
    while (true) {
        auto more = input_->increment_token();
        if (!more.has_value()) {
            return std::unexpected(std::move(more.error()));
        }
        if (!more.value()) {
            exhausted_ = true;
            return false;
        }
    }
}

const std::set<std::string, std::less<>> &ElisionFilter::french_articles() {
    static const std::set<std::string, std::less<>> articles{"l", "m", "t", "qu", "n", "s", "j", "d", "c",
                                                            "jusqu", "quoiqu", "lorsqu", "puisqu"};
    return articles;
}

ElisionFilter::ElisionFilter(std::unique_ptr<TokenStream> input, std::set<std::string, std::less<>> articles)
    : TokenFilter(std::move(input)), articles_(std::move(articles)) {
    attributes().add_attribute<CharTermAttribute>();
}

Result<bool> ElisionFilter::increment_token() {
    auto more = input_->increment_token();
    if (!more.has_value()) {
        return std::unexpected(std::move(more.error()));
    }
    if (!more.value()) {
        return false;
    }
    auto *term = attributes().get_attribute<CharTermAttribute>();
    const std::string_view text = term->text();

    // An article is matched case insensitively, and the apostrophe may be the
    // ASCII one or U+2019.
    size_t article_length = 0;
    for (size_t length = 1; length < text.size(); ++length) {
        if (text[length] != '\'' && !(length + 2 < text.size() && text.compare(length, 3, "\xE2\x80\x99") == 0)) {
            continue;
        }
        std::string candidate(text.substr(0, length));
        std::transform(candidate.begin(), candidate.end(), candidate.begin(), [](char value) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
        });
        if (articles_.contains(candidate)) {
            article_length = length + (text[length] == '\'' ? 1 : 3);
            break;
        }
    }
    if (article_length != 0 && article_length < text.size()) {
        term->set_text(std::string(text.substr(article_length)));
    }
    return true;
}

SetKeywordMarkerFilter::SetKeywordMarkerFilter(std::unique_ptr<TokenStream> input,
                                               std::set<std::string, std::less<>> keywords)
    : TokenFilter(std::move(input)), keywords_(std::move(keywords)) {
    attributes().add_attribute<CharTermAttribute>();
    attributes().add_attribute<KeywordAttribute>();
}

Result<bool> SetKeywordMarkerFilter::increment_token() {
    auto more = input_->increment_token();
    if (!more.has_value()) {
        return std::unexpected(std::move(more.error()));
    }
    if (!more.value()) {
        return false;
    }
    auto *term = attributes().get_attribute<CharTermAttribute>();
    auto *keyword = attributes().get_attribute<KeywordAttribute>();
    if (term != nullptr && keyword != nullptr) {
        // Set the flag both ways: the reference implementation relies on Lucene
        // clearing the attributes of every token, which this port does not do.
        keyword->set_keyword(keywords_.contains(term->text()));
    }
    return true;
}

} // namespace lucene
