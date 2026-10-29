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

module lucene_core:english_possessive_filter.impl;

import :english_possessive_filter;

import :attributes;
import :token_stream;
import :types;

import std;

namespace lucene {

namespace {

/// The byte length of the apostrophe at `position`, or 0 when there is none.
/// Accepts U+0027, U+2019 (typographic) and U+FF07 (full width).
[[nodiscard]] size_t apostrophe_length(std::string_view text, size_t position) {
    if (position + 3 <= text.size() && (text.substr(position, 3) == "\xE2\x80\x99" ||
                                        text.substr(position, 3) == "\xEF\xBC\x87")) {
        return 3;
    }
    if (position < text.size() && text[position] == '\'') {
        return 1;
    }
    return 0;
}

} // namespace

EnglishPossessiveFilter::EnglishPossessiveFilter(std::unique_ptr<TokenStream> input) : TokenFilter(std::move(input)) {
    attributes().add_attribute<CharTermAttribute>();
}

Result<bool> EnglishPossessiveFilter::increment_token() {
    auto more = input_->increment_token();
    if (!more.has_value()) {
        return std::unexpected(std::move(more.error()));
    }
    if (!more.value()) {
        return false;
    }

    auto *term = attributes().get_attribute<CharTermAttribute>();
    if (term == nullptr) {
        return true;
    }
    const std::string_view text = term->text();

    // Search backwards for the apostrophe; only the tail matters.
    for (size_t i = text.size(); i > 0; --i) {
        const size_t position = i - 1;
        const size_t length = apostrophe_length(text, position);
        if (length == 0) {
            continue;
        }
        // The apostrophe must be followed by nothing, `s` or `S`.
        const size_t after = position + length;
        const bool ends_with_s = (after + 1 == text.size() && (text[after] == 's' || text[after] == 'S'));
        if (after == text.size() || ends_with_s) {
            term->set_length(position);
        }
        break;
    }
    return true;
}

} // namespace lucene
