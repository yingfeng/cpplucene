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

import std;

namespace lucene {

char32_t to_lower_case(char32_t cp) noexcept {
    // ASCII.
    if (cp >= U'A' && cp <= U'Z') {
        return cp + 0x20;
    }
    // Latin-1 supplement, except the multiplication sign (U+00D7).
    if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) {
        return cp + 0x20;
    }
    // Latin Extended-A: mostly even/odd pairs, with the usual exceptions.
    if (cp >= 0x0100 && cp <= 0x017F) {
        if (cp == 0x0130) { // LATIN CAPITAL LETTER I WITH DOT ABOVE
            return 0x0069;
        }
        if ((cp & 1) == 0) {
            return cp + 1;
        }
        return cp;
    }
    // Greek.
    if (cp >= 0x0391 && cp <= 0x03A9 && cp != 0x03A2) {
        return cp + 0x20;
    }
    if (cp == 0x0386) {
        return 0x03AC;
    }
    if (cp >= 0x0388 && cp <= 0x038A) {
        return cp + 0x25;
    }
    if (cp == 0x038C) {
        return 0x03CC;
    }
    if (cp == 0x038E || cp == 0x038F) {
        return cp + 0x3F;
    }
    // Cyrillic.
    if (cp >= 0x0410 && cp <= 0x042F) {
        return cp + 0x20;
    }
    if (cp >= 0x0400 && cp <= 0x040F) {
        return cp + 0x50;
    }
    return cp;
}

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
        const char32_t lower = to_lower_case(cp);
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
