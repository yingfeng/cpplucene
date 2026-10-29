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

module lucene_core:word_delimiter.impl;

import :word_delimiter;

import :types;
import :unicode_util;

import std;

namespace lucene {

namespace {

/// Decodes UTF-8, appending the byte offset of every code point to `offsets`.
std::vector<char32_t> decode_with_offsets(std::string_view text, std::vector<int32_t> &offsets) {
    std::vector<char32_t> code_points;
    code_points.reserve(text.size());
    offsets.clear();
    offsets.reserve(text.size() + 1);
    size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        offsets.push_back(static_cast<int32_t>(i));
        if (lead < 0x80) {
            code_points.push_back(lead);
            ++i;
            continue;
        }
        size_t extra = 0;
        char32_t value = 0;
        if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            value = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
            value = lead & 0x0Fu;
        } else {
            extra = 3;
            value = lead & 0x07u;
        }
        if (i + extra >= text.size()) {
            // Truncated sequence: treat the remaining bytes as delimiters one by one.
            code_points.push_back(static_cast<char32_t>(0xFFFFFFFFu));
            ++i;
            continue;
        }
        for (size_t k = 1; k <= extra; ++k) {
            value = (value << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        }
        code_points.push_back(value);
        i += extra + 1;
    }
    offsets.push_back(static_cast<int32_t>(text.size()));
    return code_points;
}

/// Combining marks, which `Character.getType` reports as mark categories and the
/// filter therefore treats as letters.
bool is_combining_mark(char32_t cp) noexcept {
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20F0) ||
           (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0x0900 && cp <= 0x0903) ||
           (cp >= 0x093A && cp <= 0x094F) || (cp >= 0x0951 && cp <= 0x0957) ||
           (cp >= 0x0E31 && cp <= 0x0E3A) || (cp >= 0x0E47 && cp <= 0x0E4E);
}

/// Non-ASCII code points in these ranges are punctuation, symbols or separators,
/// i.e. `Character.getType` categories the filter treats as subword delimiters.
///
/// The reference implementation has the whole Unicode general-category table; this
/// port has to approximate it (see [`unicode_util`](lucene_core:unicode_util)), so
/// anything outside them is assumed to be a letter.  That direction of error is
/// the safe one: an unknown script stays one subword instead of being dropped.
bool is_punctuation_like(char32_t cp) noexcept {
    return (cp >= 0x00A0 && cp <= 0x00BF) || (cp >= 0x2000 && cp <= 0x2BFF) ||
           (cp >= 0x2E00 && cp <= 0x2E7F) || (cp >= 0x3000 && cp <= 0x303F) ||
           (cp >= 0xFE10 && cp <= 0xFE6F) || (cp >= 0xFEFF && cp <= 0xFEFF) ||
           (cp >= 0xFF00 && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) ||
           (cp >= 0xFF5B && cp <= 0xFF65) || (cp >= 0xFFF0 && cp <= 0xFFFF);
}

} // namespace

int32_t WordDelimiterIterator::get_type(char32_t codepoint) noexcept {
    if (codepoint == static_cast<char32_t>(0xFFFFFFFFu)) {
        // An undecodable byte behaves like a delimiter, so it never glues words.
        return kSubwordDelim;
    }
    if (char_is_digit(codepoint)) {
        return kDigit;
    }
    if (char_is_letter(codepoint)) {
        if (char_to_lower(codepoint) != codepoint) {
            return kUpper;
        }
        // Letters without case (CJK, Hangul, ...) are `OTHER_LETTER` in Java, which
        // the filter reports as ALPHA rather than LOWER.
        const bool cased = codepoint < 0x0250 || (codepoint >= 0x0370 && codepoint <= 0x052F) ||
                           (codepoint >= 0x1E00 && codepoint <= 0x1FFF);
        return cased ? kLower : kAlpha;
    }
    if (is_combining_mark(codepoint)) {
        return kAlpha;
    }
    if (codepoint < 0x80 || is_punctuation_like(codepoint)) {
        return kSubwordDelim;
    }
    // An unrecognised non-ASCII code point: assume a letter so the subword survives.
    return kAlpha;
}

WordDelimiterIterator::WordDelimiterIterator(std::string_view text, bool split_on_case_change,
                                             bool split_on_numerics, bool stem_english_possessive)
    : split_on_case_change_(split_on_case_change),
      split_on_numerics_(split_on_numerics),
      stem_english_possessive_(stem_english_possessive) {
    set_text(text);
}

int32_t WordDelimiterIterator::byte_offset(int32_t codepoint_index) const {
    if (codepoint_index < 0) {
        return 0;
    }
    const auto index = static_cast<size_t>(codepoint_index);
    if (index >= byte_offsets_.size()) {
        return static_cast<int32_t>(text_.size());
    }
    return byte_offsets_[index];
}

void WordDelimiterIterator::set_text(std::string_view text) {
    text_.assign(text);
    std::vector<int32_t> offsets;
    const std::vector<char32_t> code_points = decode_with_offsets(text_, offsets);
    byte_offsets_ = std::move(offsets);
    types_.clear();
    types_.reserve(code_points.size());
    for (const char32_t code_point : code_points) {
        types_.push_back(get_type(code_point));
    }

    end_bounds_ = static_cast<int32_t>(code_points.size());
    start_bounds_ = 0;
    current_ = 0;
    end_ = 0;
    skip_possessive_ = false;
    has_final_possessive_ = false;
    set_bounds();
}

int32_t WordDelimiterIterator::char_type(int32_t codepoint_index) const {
    if (codepoint_index < 0 || codepoint_index >= static_cast<int32_t>(types_.size())) {
        return kSubwordDelim;
    }
    return types_[static_cast<size_t>(codepoint_index)];
}

void WordDelimiterIterator::set_bounds() {
    while (start_bounds_ < static_cast<int32_t>(types_.size()) &&
           is_subword_delim(char_type(start_bounds_))) {
        ++start_bounds_;
    }
    while (end_bounds_ > start_bounds_ && is_subword_delim(char_type(end_bounds_ - 1))) {
        --end_bounds_;
    }
    if (ends_with_possessive(end_bounds_)) {
        has_final_possessive_ = true;
    }
    current_ = start_bounds_;
}

bool WordDelimiterIterator::ends_with_possessive(int32_t codepoint_index) const {
    return stem_english_possessive_ && codepoint_index > 2 &&
           text_[static_cast<size_t>(byte_offset(codepoint_index - 2))] == '\'' &&
           (text_[static_cast<size_t>(byte_offset(codepoint_index - 1))] == 's' ||
            text_[static_cast<size_t>(byte_offset(codepoint_index - 1))] == 'S') &&
           is_alpha(char_type(codepoint_index - 3)) &&
           (codepoint_index == end_bounds_ || is_subword_delim(char_type(codepoint_index)));
}

int32_t WordDelimiterIterator::next() {
    current_ = end_;
    if (current_ == kDone) {
        return kDone;
    }

    if (skip_possessive_) {
        current_ += 2;
        skip_possessive_ = false;
    }

    int32_t last_type = 0;
    while (current_ < end_bounds_ && is_subword_delim(last_type = char_type(current_))) {
        ++current_;
    }

    if (current_ >= end_bounds_) {
        end_ = kDone;
        return kDone;
    }

    for (end_ = current_ + 1; end_ < end_bounds_; ++end_) {
        const int32_t type = char_type(end_);
        if (is_break(last_type, type)) {
            break;
        }
        last_type = type;
    }

    if (end_ < end_bounds_ - 1 && ends_with_possessive(end_ + 2)) {
        skip_possessive_ = true;
    }
    return end_;
}

int32_t WordDelimiterIterator::type() const {
    if (end_ == kDone) {
        return 0;
    }
    const int32_t type = char_type(current_);
    // Both LOWER and UPPER are reported as ALPHA.
    if (type == kLower || type == kUpper) {
        return kAlpha;
    }
    return type;
}

int32_t WordDelimiterIterator::current() const {
    if (end_ == kDone) {
        return byte_offset(end_bounds_);
    }
    return byte_offset(current_);
}

int32_t WordDelimiterIterator::end() const {
    if (end_ == kDone) {
        return kDone;
    }
    return byte_offset(end_);
}

bool WordDelimiterIterator::is_single_word() const {
    if (has_final_possessive_) {
        return current_ == start_bounds_ && end_ == end_bounds_ - 2;
    }
    return current_ == start_bounds_ && end_ == end_bounds_;
}

bool WordDelimiterIterator::is_break(int32_t last_type, int32_t type) const {
    if ((type & last_type) != 0) {
        return false;
    }
    if (!split_on_case_change_ && is_alpha(last_type) && is_alpha(type)) {
        // ALPHA->ALPHA: ignore the case change when case is not considered.
        return false;
    }
    if (is_upper(last_type) && is_alpha(type)) {
        // UPPER->letter: don't split.
        return false;
    }
    if (!split_on_numerics_ &&
        ((is_alpha(last_type) && is_digit(type)) || (is_digit(last_type) && is_alpha(type)))) {
        // ALPHA->DIGIT, DIGIT->ALPHA: don't split.
        return false;
    }
    return true;
}

} // namespace lucene
