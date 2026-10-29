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

module lucene_core:cjk_width_filter.impl;

import :cjk_width_filter;

import :attributes;
import :string_helper;
import :token_stream;
import :types;

import std;

namespace lucene {

namespace {

/// Half width katakana `U+FF61`..`U+FF9F` to full width, in code point order.
/// The first five entries are the Japanese punctuation marks of that block.
constexpr char32_t kHalfWidthKatakana[] = {
        U'\u3002', U'\u300C', U'\u300D', U'\u3001', U'\u30FB', // 。「」、・
        U'\u30F2',                                             // ヲ
        U'\u30A1', U'\u30A3', U'\u30A5', U'\u30A7', U'\u30A9', // ァィゥェォ
        U'\u30E3', U'\u30E5', U'\u30E7', U'\u30C3',             // ャュョッ
        U'\u30FC',                                              // ー
        U'\u30A2', U'\u30A4', U'\u30A6', U'\u30A8', U'\u30AA', // アイウエオ
        U'\u30AB', U'\u30AD', U'\u30AF', U'\u30B1', U'\u30B3', // カキクケコ
        U'\u30B5', U'\u30B7', U'\u30B9', U'\u30BB', U'\u30BD', // サシスセソ
        U'\u30BF', U'\u30C1', U'\u30C4', U'\u30C6', U'\u30C8', // タチツテト
        U'\u30CA', U'\u30CB', U'\u30CC', U'\u30CD', U'\u30CE', // ナニヌネノ
        U'\u30CF', U'\u30D2', U'\u30D5', U'\u30D8', U'\u30DB', // ハヒフヘホ
        U'\u30DE', U'\u30DF', U'\u30E0', U'\u30E1', U'\u30E2', // マミムメモ
        U'\u30E4', U'\u30E6', U'\u30E8',                       // ヤユヨ
        U'\u30E9', U'\u30EA', U'\u30EB', U'\u30EC', U'\u30ED', // ラリルレロ
        U'\u30EF', U'\u30F3',                                   // ワン
        U'\u3099', U'\u309A',                                   // ゛゜
};

/// The full width form of `value`, or `value` itself.
[[nodiscard]] char32_t fold_code_point(char32_t value) {
    if (value >= U'\uFF01' && value <= U'\uFF5E') {
        // Full width ASCII: subtracting the offset gives the ASCII code point.
        return value - 0xFEE0U;
    }
    if (value == U'\u3000') {
        return U' ';
    }
    if (value >= U'\uFF61' && value <= U'\uFF9F') {
        return kHalfWidthKatakana[value - U'\uFF61'];
    }
    return value;
}

} // namespace

CJKWidthFilter::CJKWidthFilter(std::unique_ptr<TokenStream> input) : TokenFilter(std::move(input)) {
    attributes().add_attribute<CharTermAttribute>();
}

std::string CJKWidthFilter::fold(std::string_view text) {
    auto code_points = StringHelper::utf8_to_code_points(text);
    if (!code_points.has_value()) {
        return std::string(text);
    }

    bool changed = false;
    std::string folded;
    folded.reserve(text.size());
    for (const int32_t code_point : code_points.value()) {
        const char32_t value = fold_code_point(static_cast<char32_t>(code_point));
        if (value != static_cast<char32_t>(code_point)) {
            changed = true;
        }
        if (!StringHelper::utf8_append(folded, static_cast<int32_t>(value)).has_value()) {
            return std::string(text);
        }
    }
    return changed ? folded : std::string(text);
}

Result<bool> CJKWidthFilter::increment_token() {
    auto more = input_->increment_token();
    if (!more.has_value()) {
        return std::unexpected(std::move(more.error()));
    }
    if (!more.value()) {
        return false;
    }

    auto *term = attributes().get_attribute<CharTermAttribute>();
    if (term != nullptr && !term->empty()) {
        std::string folded = fold(term->text());
        if (folded != term->text()) {
            term->set_text(folded);
        }
    }
    return true;
}

} // namespace lucene
