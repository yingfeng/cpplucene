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

module lucene_core:ascii_folding_filter.impl;

import :ascii_folding_filter;

import :attributes;
import :string_helper;
import :token_stream;
import :types;

import std;

namespace lucene {

namespace {

struct Fold {
    char32_t code_point;
    const char *ascii;
};

/// Latin-1 Supplement and Latin Extended-A, in code point order.  `U+00D7`
/// (multiplication sign) and `U+00F7` (division sign) are deliberately absent:
/// they are not letters.
constexpr Fold kFolds[] = {
        // ---- Latin-1 Supplement, upper case ----
        {0x00C0, "A"}, {0x00C1, "A"}, {0x00C2, "A"}, {0x00C3, "A"}, {0x00C4, "A"}, {0x00C5, "A"},
        {0x00C6, "AE"}, {0x00C7, "C"}, {0x00C8, "E"}, {0x00C9, "E"}, {0x00CA, "E"}, {0x00CB, "E"},
        {0x00CC, "I"}, {0x00CD, "I"}, {0x00CE, "I"}, {0x00CF, "I"}, {0x00D0, "D"}, {0x00D1, "N"},
        {0x00D2, "O"}, {0x00D3, "O"}, {0x00D4, "O"}, {0x00D5, "O"}, {0x00D6, "O"}, {0x00D8, "O"},
        {0x00D9, "U"}, {0x00DA, "U"}, {0x00DB, "U"}, {0x00DC, "U"}, {0x00DD, "Y"}, {0x00DE, "TH"},
        {0x00DF, "ss"},
        // ---- Latin-1 Supplement, lower case ----
        {0x00E0, "a"}, {0x00E1, "a"}, {0x00E2, "a"}, {0x00E3, "a"}, {0x00E4, "a"}, {0x00E5, "a"},
        {0x00E6, "ae"}, {0x00E7, "c"}, {0x00E8, "e"}, {0x00E9, "e"}, {0x00EA, "e"}, {0x00EB, "e"},
        {0x00EC, "i"}, {0x00ED, "i"}, {0x00EE, "i"}, {0x00EF, "i"}, {0x00F0, "d"}, {0x00F1, "n"},
        {0x00F2, "o"}, {0x00F3, "o"}, {0x00F4, "o"}, {0x00F5, "o"}, {0x00F6, "o"}, {0x00F8, "o"},
        {0x00F9, "u"}, {0x00FA, "u"}, {0x00FB, "u"}, {0x00FC, "u"}, {0x00FD, "y"}, {0x00FE, "th"},
        {0x00FF, "y"},
        // ---- Latin Extended-A ----
        {0x0100, "A"}, {0x0101, "a"}, {0x0102, "A"}, {0x0103, "a"}, {0x0104, "A"}, {0x0105, "a"},
        {0x0106, "C"}, {0x0107, "c"}, {0x0108, "C"}, {0x0109, "c"}, {0x010A, "C"}, {0x010B, "c"},
        {0x010C, "C"}, {0x010D, "c"}, {0x010E, "D"}, {0x010F, "d"}, {0x0110, "D"}, {0x0111, "d"},
        {0x0112, "E"}, {0x0113, "e"}, {0x0114, "E"}, {0x0115, "e"}, {0x0116, "E"}, {0x0117, "e"},
        {0x0118, "E"}, {0x0119, "e"}, {0x011A, "E"}, {0x011B, "e"}, {0x011C, "G"}, {0x011D, "g"},
        {0x011E, "G"}, {0x011F, "g"}, {0x0120, "G"}, {0x0121, "g"}, {0x0122, "G"}, {0x0123, "g"},
        {0x0124, "H"}, {0x0125, "h"}, {0x0126, "H"}, {0x0127, "h"}, {0x0128, "I"}, {0x0129, "i"},
        {0x012A, "I"}, {0x012B, "i"}, {0x012C, "I"}, {0x012D, "i"}, {0x012E, "I"}, {0x012F, "i"},
        {0x0130, "I"}, {0x0131, "i"}, {0x0132, "IJ"}, {0x0133, "ij"}, {0x0134, "J"}, {0x0135, "j"},
        {0x0136, "K"}, {0x0137, "k"}, {0x0138, "k"}, {0x0139, "L"}, {0x013A, "l"}, {0x013B, "L"},
        {0x013C, "l"}, {0x013D, "L"}, {0x013E, "l"}, {0x013F, "L"}, {0x0140, "l"}, {0x0141, "L"},
        {0x0142, "l"}, {0x0143, "N"}, {0x0144, "n"}, {0x0145, "N"}, {0x0146, "n"}, {0x0147, "N"},
        {0x0148, "n"}, {0x0149, "'n"}, {0x014A, "NG"}, {0x014B, "ng"}, {0x014C, "O"}, {0x014D, "o"},
        {0x014E, "O"}, {0x014F, "o"}, {0x0150, "O"}, {0x0151, "o"}, {0x0152, "OE"}, {0x0153, "oe"},
        {0x0154, "R"}, {0x0155, "r"}, {0x0156, "R"}, {0x0157, "r"}, {0x0158, "R"}, {0x0159, "r"},
        {0x015A, "S"}, {0x015B, "s"}, {0x015C, "S"}, {0x015D, "s"}, {0x015E, "S"}, {0x015F, "s"},
        {0x0160, "S"}, {0x0161, "s"}, {0x0162, "T"}, {0x0163, "t"}, {0x0164, "T"}, {0x0165, "t"},
        {0x0166, "T"}, {0x0167, "t"}, {0x0168, "U"}, {0x0169, "u"}, {0x016A, "U"}, {0x016B, "u"},
        {0x016C, "U"}, {0x016D, "u"}, {0x016E, "U"}, {0x016F, "u"}, {0x0170, "U"}, {0x0171, "u"},
        {0x0172, "U"}, {0x0173, "u"}, {0x0174, "W"}, {0x0175, "w"}, {0x0176, "Y"}, {0x0177, "y"},
        {0x0178, "Y"}, {0x0179, "Z"}, {0x017A, "z"}, {0x017B, "Z"}, {0x017C, "z"}, {0x017D, "Z"},
        {0x017E, "z"}, {0x017F, "s"},
};

[[nodiscard]] const char *fold_lookup(char32_t code_point) {
    const auto *end = kFolds + std::size(kFolds);
    const auto *found = std::lower_bound(kFolds, end, code_point,
                                         [](const Fold &fold, char32_t value) { return fold.code_point < value; });
    if (found != end && found->code_point == code_point) {
        return found->ascii;
    }
    return nullptr;
}

} // namespace

ASCIIFoldingFilter::ASCIIFoldingFilter(std::unique_ptr<TokenStream> input, bool preserve_original)
    : TokenFilter(std::move(input)), preserve_original_(preserve_original) {
    attributes().add_attribute<CharTermAttribute>();
    attributes().add_attribute<PositionIncrementAttribute>();
}

VoidResult ASCIIFoldingFilter::reset() {
    pending_.clear();
    has_pending_ = false;
    return TokenFilter::reset();
}

std::string ASCIIFoldingFilter::fold(std::string_view text) {
    auto code_points = StringHelper::utf8_to_code_points(text);
    if (!code_points.has_value()) {
        return std::string(text);
    }

    bool changed = false;
    std::string folded;
    folded.reserve(text.size());
    for (const int32_t code_point : code_points.value()) {
        if (const char *replacement = fold_lookup(static_cast<char32_t>(code_point)); replacement != nullptr) {
            folded += replacement;
            changed = true;
            continue;
        }
        if (!StringHelper::utf8_append(folded, code_point).has_value()) {
            return std::string(text);
        }
    }
    return changed ? folded : std::string(text);
}

Result<bool> ASCIIFoldingFilter::increment_token() {
    auto *term = attributes().get_attribute<CharTermAttribute>();
    auto *position_increment = attributes().get_attribute<PositionIncrementAttribute>();

    if (has_pending_) {
        term->set_text(pending_);
        position_increment->set_position_increment(0);
        pending_.clear();
        has_pending_ = false;
        return true;
    }

    auto more = input_->increment_token();
    if (!more.has_value()) {
        return std::unexpected(std::move(more.error()));
    }
    if (!more.value()) {
        return false;
    }

    const std::string original(term->text());
    std::string folded = fold(original);
    if (folded != original) {
        term->set_text(folded);
        if (preserve_original_) {
            pending_ = original;
            has_pending_ = true;
        }
    }
    return true;
}

} // namespace lucene
