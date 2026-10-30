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

module lucene_core:standard_tokenizer.impl;

import :standard_tokenizer;
import :types;
import :error;
import :attributes;
import :tokenizer;
import :unicode_util;

import std;

namespace lucene {

namespace {

/// A half-open code point interval of a Unicode property.
struct Range {
    char32_t first;
    char32_t last;
};

/// Binary search over a sorted, non-overlapping interval table.
constexpr bool in_ranges(char32_t cp, std::span<const Range> ranges) noexcept {
    size_t low = 0;
    size_t high = ranges.size();
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        if (cp < ranges[mid].first) {
            high = mid;
        } else if (cp > ranges[mid].last) {
            low = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

// ---- UAX #29 property tables (the subset needed by the standard tokenizer) ----
//
// The tables are hand-maintained transcriptions of
// `WordBreakProperty.txt` (Unicode 15) restricted to the ranges a tokenizer can
// encounter, which keeps the module free of generated megabytes while every
// entry stays reviewable.  Ranges must be sorted and must not overlap.

constexpr Range kExtend[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2},
    {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670},
    {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711},
    {0x0730, 0x074A}, {0x07A6, 0x07B0}, {0x0900, 0x0903}, {0x093A, 0x093C}, {0x093E, 0x094F},
    {0x0951, 0x0957}, {0x0962, 0x0963}, {0x0981, 0x0983}, {0x09BC, 0x09BC}, {0x09BE, 0x09C4},
    {0x09D7, 0x09D7}, {0x0A01, 0x0A03}, {0x0B01, 0x0B03}, {0x0B3C, 0x0B3C}, {0x0B3E, 0x0B44},
    {0x0C00, 0x0C04}, {0x0D00, 0x0D03}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},
    {0x0EB1, 0x0EB1}, {0x0F71, 0x0F84}, {0x102B, 0x103E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF},
    {0x20D0, 0x20F0}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0x101FD, 0x101FD}, {0x1D165, 0x1D169},
    {0x1D16D, 0x1D182}, {0x1D185, 0x1D18B}, {0x1D1AA, 0x1D1AD}, {0xE0100, 0xE01EF},
};

constexpr Range kFormat[] = {
    {0x00AD, 0x00AD}, {0x0600, 0x0605}, {0x061C, 0x061C}, {0x06DD, 0x06DD}, {0x070F, 0x070F},
    {0x180E, 0x180E}, {0x200B, 0x200C}, {0x200E, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064},
    {0x2066, 0x206F}, {0xFEFF, 0xFEFF}, {0xFFF9, 0xFFFB}, {0x1D173, 0x1D17A},
};

constexpr Range kIdeographic[] = {
    {0x3006, 0x3007},   {0x3021, 0x3029},   {0x3038, 0x303A},   {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},
    {0xF900, 0xFA6D},   {0xFA70, 0xFAD9},   {0x20000, 0x2A6DF}, {0x2A700, 0x2EBE0}, {0x2F800, 0x2FA1D},
    {0x30000, 0x3134A},
};

constexpr Range kHiragana[] = {{0x3041, 0x3096}, {0x309D, 0x309F}};

constexpr Range kKatakana[] = {
    {0x30A1, 0x30FA}, {0x30FD, 0x30FF}, {0x31F0, 0x31FF}, {0x32D0, 0x32FE},
    {0x3300, 0x3357}, {0xFF66, 0xFF6F}, {0xFF71, 0xFF9D},
};

constexpr Range kHangul[] = {
    {0x1100, 0x11FF}, {0x3131, 0x318E}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xD7B0, 0xD7FB},
};

// Skin-tone modifiers (U+1F3FB..U+1F3FF) fall inside the U+1F300..U+1F5FF
// block, so they need no separate entry.
constexpr Range kEmoji[] = {
    {0x2600, 0x27BF},   {0x2B00, 0x2BFF},   {0x1F000, 0x1F02F}, {0x1F0A0, 0x1F0FF}, {0x1F100, 0x1F1AD},
    {0x1F300, 0x1F5FF}, {0x1F600, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF},
};

constexpr Range kRegionalIndicator[] = {{0x1F1E6, 0x1F1FF}};

constexpr Range kMidLetter[] = {
    {0x003A, 0x003A}, {0x00B7, 0x00B7}, {0x0387, 0x0387}, {0x05F4, 0x05F4},
    {0x2027, 0x2027}, {0xFE13, 0xFE13}, {0xFE55, 0xFE55}, {0xFF1A, 0xFF1A},
};

constexpr Range kMidNum[] = {
    {0x002C, 0x002C}, {0x003B, 0x003B}, {0x037E, 0x037E}, {0x0589, 0x0589}, {0x060C, 0x060D},
    {0x066C, 0x066C}, {0x07F8, 0x07F8}, {0x2044, 0x2044}, {0xFE10, 0xFE10}, {0xFE14, 0xFE14},
    {0xFE50, 0xFE50}, {0xFE54, 0xFE54}, {0xFF0C, 0xFF0C}, {0xFF1B, 0xFF1B},
};

constexpr Range kMidNumLet[] = {
    {0x0027, 0x0027}, {0x002E, 0x002E}, {0x2018, 0x2019}, {0x2024, 0x2024},
    {0xFE52, 0xFE52}, {0xFF07, 0xFF07}, {0xFF0E, 0xFF0E},
};

constexpr Range kExtendNumLet[] = {
    {0x005F, 0x005F}, {0x203F, 0x2040}, {0x2054, 0x2054},
    {0xFE33, 0xFE34}, {0xFE4D, 0xFE4F}, {0xFF3F, 0xFF3F},
};

/// `WB4`: `Extend` and `Format` never break, they glue to the previous token.
constexpr bool is_word_continuation(WordBreakClass cls) noexcept {
    return cls == WordBreakClass::Extend || cls == WordBreakClass::Format;
}

/// `in_ranges` binary-searches, so every table must be sorted and disjoint;
/// checked at compile time so a mis-transcribed entry cannot hide in a rarely
/// exercised range.
template <size_t N>
constexpr bool is_sorted_table(const Range (&ranges)[N]) noexcept {
    for (size_t i = 1; i < N; ++i) {
        if (ranges[i].first <= ranges[i - 1].last) {
            return false;
        }
    }
    return true;
}

static_assert(is_sorted_table(kExtend));
static_assert(is_sorted_table(kFormat));
static_assert(is_sorted_table(kIdeographic));
static_assert(is_sorted_table(kHiragana));
static_assert(is_sorted_table(kKatakana));
static_assert(is_sorted_table(kHangul));
static_assert(is_sorted_table(kEmoji));
static_assert(is_sorted_table(kRegionalIndicator));
static_assert(is_sorted_table(kMidLetter));
static_assert(is_sorted_table(kMidNum));
static_assert(is_sorted_table(kMidNumLet));
static_assert(is_sorted_table(kExtendNumLet));

/// `true` when a token may start at `cls` (punctuation, whitespace and
/// combining marks never do).
constexpr bool is_word_start(WordBreakClass cls) noexcept {
    return cls == WordBreakClass::ALetter || cls == WordBreakClass::Numeric ||
           cls == WordBreakClass::Ideographic || cls == WordBreakClass::Hiragana ||
           cls == WordBreakClass::Katakana || cls == WordBreakClass::Hangul || cls == WordBreakClass::Emoji ||
           cls == WordBreakClass::RegionalIndicator || cls == WordBreakClass::ExtendNumLet;
}

} // namespace

namespace {

/// The class of every code point, by the range scans below.
WordBreakClass word_break_class_slow(char32_t cp) noexcept {
    if (char_is_whitespace(cp)) {
        return WordBreakClass::Other;
    }
    if (cp == 0x200D) {
        return WordBreakClass::Zwj;
    }
    // Order matters: the script classes must win over the generic letter class.
    if (in_ranges(cp, kIdeographic)) {
        return WordBreakClass::Ideographic;
    }
    if (in_ranges(cp, kHiragana)) {
        return WordBreakClass::Hiragana;
    }
    if (in_ranges(cp, kKatakana)) {
        return WordBreakClass::Katakana;
    }
    if (in_ranges(cp, kHangul)) {
        return WordBreakClass::Hangul;
    }
    if (in_ranges(cp, kRegionalIndicator)) {
        return WordBreakClass::RegionalIndicator;
    }
    if (in_ranges(cp, kEmoji)) {
        return WordBreakClass::Emoji;
    }
    if (in_ranges(cp, kExtend)) {
        return WordBreakClass::Extend;
    }
    if (in_ranges(cp, kFormat)) {
        return WordBreakClass::Format;
    }
    if (char_is_digit(cp)) {
        return WordBreakClass::Numeric;
    }
    if (char_is_letter(cp)) {
        return WordBreakClass::ALetter;
    }
    if (in_ranges(cp, kMidLetter)) {
        return WordBreakClass::MidLetter;
    }
    if (in_ranges(cp, kMidNum)) {
        return WordBreakClass::MidNum;
    }
    if (in_ranges(cp, kMidNumLet)) {
        return WordBreakClass::MidNumLet;
    }
    if (in_ranges(cp, kExtendNumLet)) {
        return WordBreakClass::ExtendNumLet;
    }
    return WordBreakClass::Other;
}

/// ASCII answers from a table: the slow path binary-searches a dozen sorted
/// tables, which every character of a query token paid for.  Built from the same
/// function, so the classification cannot drift from the full one.
const std::array<WordBreakClass, 0x80> &ascii_classes() {
    static const std::array<WordBreakClass, 0x80> table = [] {
        std::array<WordBreakClass, 0x80> built{};
        for (size_t i = 0; i < built.size(); ++i) {
            built[i] = word_break_class_slow(static_cast<char32_t>(i));
        }
        return built;
    }();
    return table;
}

} // namespace

WordBreakClass word_break_class(char32_t cp) noexcept {
    if (cp < 0x80) {
        return ascii_classes()[static_cast<size_t>(cp)];
    }
    return word_break_class_slow(cp);
}

StandardTokenizer::StandardTokenizer(size_t max_token_length)
    : max_token_length_(max_token_length == 0 ? DEFAULT_MAX_TOKEN_LENGTH : max_token_length) {
    if (max_token_length_ > MAX_TOKEN_LENGTH_LIMIT) {
        max_token_length_ = MAX_TOKEN_LENGTH_LIMIT;
    }
}

VoidResult StandardTokenizer::set_max_token_length(size_t length) {
    if (length < 1) {
        return err(LuceneError::illegal_argument("maxTokenLength must be greater than zero"));
    }
    if (length > MAX_TOKEN_LENGTH_LIMIT) {
        return err(LuceneError::illegal_argument("maxTokenLength may not exceed " +
                                                std::to_string(MAX_TOKEN_LENGTH_LIMIT)));
    }
    max_token_length_ = length;
    return {};
}

void StandardTokenizer::scan_token(size_t start, size_t &end, std::string_view &type) const noexcept {
    const size_t count = code_point_count();
    const WordBreakClass first = word_break_class(code_point_at(start));
    end = start + 1;
    type = StandardTokenizerTypes::ALPHANUM;

    switch (first) {
        case WordBreakClass::Ideographic:
            // Lucene emits one token per ideograph (WB999 keeps them apart).
            type = StandardTokenizerTypes::IDEOGRAPHIC;
            return;

        case WordBreakClass::Hiragana:
        case WordBreakClass::Katakana:
        case WordBreakClass::Hangul: {
            // Lucene keeps a whole script run together (`WB13a`-ish), the UAX
            // rules would split each syllable.
            type = first == WordBreakClass::Hiragana   ? StandardTokenizerTypes::HIRAGANA
                   : first == WordBreakClass::Katakana ? StandardTokenizerTypes::KATAKANA
                                                       : StandardTokenizerTypes::HANGUL;
            while (end < count) {
                const WordBreakClass cls = word_break_class(code_point_at(end));
                if (cls == first || is_word_continuation(cls)) {
                    ++end;
                    continue;
                }
                break;
            }
            return;
        }

        case WordBreakClass::Emoji:
        case WordBreakClass::RegionalIndicator: {
            // `WB3c`/`WB12`-`WB15`: ZWJ sequences, skin tones and flag pairs
            // stay in one token.
            type = StandardTokenizerTypes::EMOJI;
            while (end < count) {
                const WordBreakClass cls = word_break_class(code_point_at(end));
                if (cls == WordBreakClass::Emoji || cls == WordBreakClass::RegionalIndicator ||
                    cls == WordBreakClass::Zwj || is_word_continuation(cls)) {
                    ++end;
                    continue;
                }
                break;
            }
            return;
        }

        case WordBreakClass::ALetter:
        case WordBreakClass::Numeric: {
            bool has_letter = first == WordBreakClass::ALetter;
            WordBreakClass previous = first;
            while (end < count) {
                const WordBreakClass cls = word_break_class(code_point_at(end));
                if (is_word_continuation(cls) || cls == WordBreakClass::ExtendNumLet) {
                    // WB4 / WB13a / WB13b: marks and `_` glue to either side.
                    ++end;
                    continue;
                }
                // WB5 (ALetter x ALetter), WB8 (Numeric x Numeric),
                // WB9 (ALetter x Numeric) and WB10 (Numeric x ALetter).
                if ((previous == WordBreakClass::ALetter &&
                     (cls == WordBreakClass::ALetter || cls == WordBreakClass::Numeric)) ||
                    (previous == WordBreakClass::Numeric &&
                     (cls == WordBreakClass::Numeric || cls == WordBreakClass::ALetter))) {
                    if (cls == WordBreakClass::ALetter) {
                        has_letter = true;
                    }
                    previous = cls;
                    ++end;
                    continue;
                }
                // WB6/WB7/WB11/WB12: `X (MidX) X`, where the look-ahead must
                // skip continuation marks (`don't`, `3.14`).
                const bool mid_ok =
                    (previous == WordBreakClass::ALetter &&
                     (cls == WordBreakClass::MidLetter || cls == WordBreakClass::MidNumLet)) ||
                    (previous == WordBreakClass::Numeric &&
                     (cls == WordBreakClass::MidNum || cls == WordBreakClass::MidNumLet));
                if (mid_ok) {
                    size_t lookahead = end + 1;
                    while (lookahead < count &&
                           is_word_continuation(word_break_class(code_point_at(lookahead)))) {
                        ++lookahead;
                    }
                    if (lookahead < count) {
                        const WordBreakClass next = word_break_class(code_point_at(lookahead));
                        const bool joins =
                            (previous == WordBreakClass::ALetter && next == WordBreakClass::ALetter) ||
                            (previous == WordBreakClass::Numeric && next == WordBreakClass::Numeric);
                        if (joins) {
                            if (next == WordBreakClass::ALetter) {
                                has_letter = true;
                            }
                            previous = next;
                            end = lookahead + 1;
                            continue;
                        }
                    }
                }
                break;
            }
            type = has_letter ? StandardTokenizerTypes::ALPHANUM : StandardTokenizerTypes::NUM;
            return;
        }

        default:
            // `ExtendNumLet` at the start of a token (`_foo`): `WB13a` glues the
            // `_` to the adjacent run, which the ALetter/Numeric rules scan.
            type = StandardTokenizerTypes::ALPHANUM;
            while (end < count) {
                const WordBreakClass cls = word_break_class(code_point_at(end));
                if (cls == WordBreakClass::ExtendNumLet || is_word_continuation(cls)) {
                    ++end;
                    continue;
                }
                if (cls == WordBreakClass::ALetter || cls == WordBreakClass::Numeric) {
                    size_t sub_end = end;
                    std::string_view sub_type;
                    scan_token(end, sub_end, sub_type);
                    end = sub_end;
                    continue;
                }
                break;
            }
            return;
    }
}

Result<bool> StandardTokenizer::increment_token() {
    const size_t count = code_point_count();
    size_t skipped = 0;

    while (true) {
        size_t start = position_;
        while (start < count && !is_word_start(word_break_class(code_point_at(start)))) {
            // `Extend`/`Format` here cannot happen right after a token (the
            // scan below consumes them), so they are skipped like punctuation.
            ++start;
        }
        if (start >= count) {
            position_ = count;
            return false;
        }

        size_t end = start + 1;
        std::string_view type = StandardTokenizerTypes::ALPHANUM;
        scan_token(start, end, type);
        position_ = end;

        if (end - start > max_token_length_) {
            // Lucene drops over-long tokens and folds their position into the
            // next token's increment instead of truncating them.
            ++skipped;
            continue;
        }

        if (!emit_token(start, end)) {
            return false;
        }
        if (auto *type_attribute = attributes_.get_attribute<TypeAttribute>(); type_attribute != nullptr) {
            type_attribute->set_type(type);
        }
        if (skipped > 0) {
            if (auto *position_increment = attributes_.get_attribute<PositionIncrementAttribute>();
                position_increment != nullptr) {
                position_increment->set_position_increment(static_cast<uint32_t>(skipped) + 1);
            }
        }
        return true;
    }
}

} // namespace lucene
