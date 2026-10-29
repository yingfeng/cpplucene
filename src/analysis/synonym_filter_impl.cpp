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

module lucene_core:synonym_filter.impl;

import :synonym_filter;

import :attributes;
import :string_helper;
import :synonym_map;
import :unicode_util;
import :token_stream;
import :types;

import std;

namespace lucene {

SynonymFilter::SynonymFilter(std::unique_ptr<TokenStream> input, const SynonymMap &map, bool ignore_case)
    : TokenFilter(std::move(input)), map_(&map), ignore_case_(ignore_case) {
    attributes().add_attribute<CharTermAttribute>();
    attributes().add_attribute<PositionIncrementAttribute>();
}

VoidResult SynonymFilter::reset() {
    buffer_.clear();
    output_.clear();
    exhausted_ = false;
    return TokenFilter::reset();
}

std::string SynonymFilter::normalize(std::string_view term) const {
    if (!ignore_case_) {
        return std::string(term);
    }
    auto code_points = StringHelper::utf8_to_code_points(term);
    if (!code_points.has_value()) {
        return std::string(term);
    }
    std::string lowered;
    lowered.reserve(term.size());
    for (const int32_t code_point : code_points.value()) {
        if (!StringHelper::utf8_append(lowered, static_cast<int32_t>(
                                                        char_to_lower(static_cast<char32_t>(code_point))))
                     .has_value()) {
            return std::string(term);
        }
    }
    return lowered;
}

Result<bool> SynonymFilter::increment_token() {
    auto *term = attributes().get_attribute<CharTermAttribute>();
    auto *position_increment = attributes().get_attribute<PositionIncrementAttribute>();

    while (output_.empty()) {
        if (buffer_.empty() && exhausted_) {
            return false;
        }

        // Keep enough lookahead for the longest rule.
        if (!exhausted_ && buffer_.size() < map_->max_input_length()) {
            auto more = input_->increment_token();
            if (!more.has_value()) {
                return std::unexpected(std::move(more.error()));
            }
            if (!more.value()) {
                exhausted_ = true;
            } else {
                buffer_.push_back(BufferedToken{std::string(term->text()), position_increment->position_increment()});
            }
            continue;
        }
        if (buffer_.empty()) {
            return false;
        }

        // Longest match wins.
        const size_t longest = std::min(buffer_.size(), map_->max_input_length());
        const std::vector<std::vector<std::string>> *alternatives = nullptr;
        size_t matched = 0;
        for (size_t length = longest; length >= 1; --length) {
            std::vector<std::string> words;
            words.reserve(length);
            for (size_t i = 0; i < length; ++i) {
                words.push_back(normalize(buffer_[i].term));
            }
            if (const auto *found = map_->lookup(words); found != nullptr) {
                alternatives = found;
                matched = length;
                break;
            }
            if (length == 1) {
                break;
            }
        }

        const uint32_t first_position_increment = buffer_.front().position_increment;
        if (alternatives == nullptr || alternatives->empty()) {
            output_.push_back(OutputToken{std::move(buffer_.front().term), first_position_increment});
            buffer_.pop_front();
            continue;
        }

        bool first = true;
        for (const std::vector<std::string> &alternative : *alternatives) {
            for (const std::string &word : alternative) {
                output_.push_back(OutputToken{word, first ? first_position_increment : 0});
                first = false;
            }
        }
        for (size_t i = 0; i < matched; ++i) {
            buffer_.pop_front();
        }
    }

    OutputToken next = std::move(output_.front());
    output_.pop_front();
    term->set_text(next.term);
    position_increment->set_position_increment(next.position_increment);
    return true;
}

} // namespace lucene
