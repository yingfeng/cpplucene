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

module lucene_core:lower_case_filter.impl;

import :lower_case_filter;
import :types;
import :error;
import :attributes;
import :token_stream;
import :unicode_util;

import std;

namespace lucene {

LowerCaseFilter::LowerCaseFilter(std::unique_ptr<TokenStream> input) : TokenFilter(std::move(input)) {
    attributes().add_attribute<CharTermAttribute>();
}

Result<bool> LowerCaseFilter::increment_token() {
    auto more = input_->increment_token();
    if (!more) {
        return std::unexpected(std::move(more.error()));
    }
    if (!more.value()) {
        return false;
    }
    auto *term = attributes().get_attribute<CharTermAttribute>();
    debug_check(term != nullptr);
    const std::u32string code_points = term->to_u32string();
    std::string lowered;
    lowered.reserve(term->length());
    for (const char32_t cp : code_points) {
        const char32_t lower = char_to_lower(cp);
        const size_t extra = lower < 0x80 ? 1 : lower < 0x800 ? 2 : lower < 0x10000 ? 3 : 4;
        if (extra == 1) {
            lowered.push_back(static_cast<char>(lower));
        } else if (extra == 2) {
            lowered.push_back(static_cast<char>(0xC0 | (lower >> 6)));
            lowered.push_back(static_cast<char>(0x80 | (lower & 0x3F)));
        } else if (extra == 3) {
            lowered.push_back(static_cast<char>(0xE0 | (lower >> 12)));
            lowered.push_back(static_cast<char>(0x80 | ((lower >> 6) & 0x3F)));
            lowered.push_back(static_cast<char>(0x80 | (lower & 0x3F)));
        } else {
            lowered.push_back(static_cast<char>(0xF0 | (lower >> 18)));
            lowered.push_back(static_cast<char>(0x80 | ((lower >> 12) & 0x3F)));
            lowered.push_back(static_cast<char>(0x80 | ((lower >> 6) & 0x3F)));
            lowered.push_back(static_cast<char>(0x80 | (lower & 0x3F)));
        }
    }
    term->set_text(lowered);
    return true;
}

} // namespace lucene
