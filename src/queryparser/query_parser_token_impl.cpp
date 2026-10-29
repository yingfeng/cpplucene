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

module lucene_core:query_parser_token.impl;

import :query_parser_token;

import :error;
import :query_parser_constants;
import :types;

import std;

namespace lucene {

namespace {

/// One decoded code point with its position in the input bytes.
struct CodePoint {
    char32_t value{0};
    size_t offset{0};
    size_t length{1};
};

/// Decodes UTF-8 leniently: a byte sequence that is not valid UTF-8 is taken as
/// one code point per byte, which keeps the lexer total instead of making an
/// unrelated encoding bug look like a syntax error.
std::vector<CodePoint> decode_utf8(std::string_view input) {
    std::vector<CodePoint> code_points;
    code_points.reserve(input.size());

    const auto continuation = [&](size_t index) {
        return index < input.size() && (static_cast<unsigned char>(input[index]) & 0xC0U) == 0x80U;
    };

    size_t i = 0;
    while (i < input.size()) {
        const auto first = static_cast<unsigned char>(input[i]);
        size_t length = 1;
        char32_t value = first;

        if (first >= 0xF0U && continuation(i + 1) && continuation(i + 2) && continuation(i + 3)) {
            value = ((static_cast<char32_t>(first) & 0x07U) << 18U) |
                    ((static_cast<char32_t>(static_cast<unsigned char>(input[i + 1])) & 0x3FU) << 12U) |
                    ((static_cast<char32_t>(static_cast<unsigned char>(input[i + 2])) & 0x3FU) << 6U) |
                    (static_cast<char32_t>(static_cast<unsigned char>(input[i + 3])) & 0x3FU);
            length = 4;
        } else if (first >= 0xE0U && continuation(i + 1) && continuation(i + 2)) {
            value = ((static_cast<char32_t>(first) & 0x0FU) << 12U) |
                    ((static_cast<char32_t>(static_cast<unsigned char>(input[i + 1])) & 0x3FU) << 6U) |
                    (static_cast<char32_t>(static_cast<unsigned char>(input[i + 2])) & 0x3FU);
            length = 3;
        } else if (first >= 0xC0U && continuation(i + 1)) {
            value = ((static_cast<char32_t>(first) & 0x1FU) << 6U) |
                    (static_cast<char32_t>(static_cast<unsigned char>(input[i + 1])) & 0x3FU);
            length = 2;
        }

        code_points.push_back(CodePoint{value, i, length});
        i += length;
    }
    return code_points;
}

void append_utf8(std::string &out, char32_t value) {
    if (value < 0x80U) {
        out.push_back(static_cast<char>(value));
    } else if (value < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (value >> 6U)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if (value < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (value >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (value >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    }
}

using CodePoints = std::vector<CodePoint>;

[[nodiscard]] bool at_end(const CodePoints &cps, size_t pos) noexcept { return pos >= cps.size(); }

[[nodiscard]] char32_t at(const CodePoints &cps, size_t pos) noexcept { return cps[pos].value; }

[[nodiscard]] bool is_digit(char32_t value) noexcept { return value >= U'0' && value <= U'9'; }

/// `_WHITESPACE`: space, tab, CR, LF and the ideographic space.
[[nodiscard]] bool is_whitespace(char32_t value) noexcept {
    return value == U' ' || value == U'\t' || value == U'\n' || value == U'\r' || value == U'\u3000';
}

/// The characters `_TERM_START_CHAR` excludes.
[[nodiscard]] bool is_term_start_excluded(char32_t value) noexcept {
    switch (value) {
        case U' ':
        case U'\t':
        case U'\n':
        case U'\r':
        case U'\u3000':
        case U'+':
        case U'-':
        case U'!':
        case U'(':
        case U')':
        case U':':
        case U'^':
        case U'[':
        case U']':
        case U'"':
        case U'{':
        case U'}':
        case U'~':
        case U'*':
        case U'?':
        case U'\\':
        case U'/':
            return true;
        default:
            return false;
    }
}

/// `_ESCAPED_CHAR`: a backslash plus one character (which may be any character,
/// the newline included, and counts as two code points).
[[nodiscard]] bool matches_escaped_char(const CodePoints &cps, size_t pos) noexcept {
    return !at_end(cps, pos) && at(cps, pos) == U'\\' && !at_end(cps, pos + 1);
}

[[nodiscard]] size_t match_term_start_char(const CodePoints &cps, size_t pos) noexcept {
    if (at_end(cps, pos)) {
        return 0;
    }
    if (matches_escaped_char(cps, pos)) {
        return 2;
    }
    return is_term_start_excluded(at(cps, pos)) ? 0 : 1;
}

[[nodiscard]] size_t match_term_char(const CodePoints &cps, size_t pos) noexcept {
    if (at_end(cps, pos)) {
        return 0;
    }
    if (matches_escaped_char(cps, pos)) {
        return 2;
    }
    if (at(cps, pos) == U'-' || at(cps, pos) == U'+') {
        return 1;
    }
    return is_term_start_excluded(at(cps, pos)) ? 0 : 1;
}

[[nodiscard]] size_t match_literal(const CodePoints &cps, size_t pos, std::u32string_view literal) noexcept {
    if (pos + literal.size() > cps.size()) {
        return 0;
    }
    for (size_t i = 0; i < literal.size(); ++i) {
        if (at(cps, pos + i) != literal[i]) {
            return 0;
        }
    }
    return literal.size();
}

// ---------------------------------------------------------------------------
// Token rules, in declaration order (a tie on length goes to the earlier rule)
// ---------------------------------------------------------------------------

[[nodiscard]] size_t match_and(const CodePoints &cps, size_t pos) noexcept {
    return std::max(match_literal(cps, pos, U"AND"), match_literal(cps, pos, U"&&"));
}

[[nodiscard]] size_t match_or(const CodePoints &cps, size_t pos) noexcept {
    return std::max(match_literal(cps, pos, U"OR"), match_literal(cps, pos, U"||"));
}

[[nodiscard]] size_t match_not(const CodePoints &cps, size_t pos) noexcept {
    return std::max(match_literal(cps, pos, U"NOT"), match_literal(cps, pos, U"!"));
}

/// `<BAREOPER: ("+"|"-"|"!") <_WHITESPACE> >` — the image keeps the trailing blank.
[[nodiscard]] size_t match_bare_oper(const CodePoints &cps, size_t pos) noexcept {
    if (at_end(cps, pos)) {
        return 0;
    }
    const char32_t op = at(cps, pos);
    if (op != U'+' && op != U'-' && op != U'!') {
        return 0;
    }
    if (at_end(cps, pos + 1) || !is_whitespace(at(cps, pos + 1))) {
        return 0;
    }
    return 2;
}

/// `<QUOTED: "\"" (<_QUOTED_CHAR>)* "\"">`: a quoted run with no unescaped quote
/// inside, so the first unescaped quote closes it.
[[nodiscard]] size_t match_quoted(const CodePoints &cps, size_t pos) noexcept {
    if (at_end(cps, pos) || at(cps, pos) != U'"') {
        return 0;
    }
    size_t i = pos + 1;
    while (i < cps.size()) {
        if (at(cps, i) == U'\\') {
            if (i + 1 >= cps.size()) {
                return 0;
            }
            i += 2;
            continue;
        }
        if (at(cps, i) == U'"') {
            return i - pos + 1;
        }
        ++i;
    }
    return 0;
}

[[nodiscard]] size_t match_term(const CodePoints &cps, size_t pos) noexcept {
    const size_t start = match_term_start_char(cps, pos);
    if (start == 0) {
        return 0;
    }
    size_t length = start;
    while (true) {
        const size_t next = match_term_char(cps, pos + length);
        if (next == 0) {
            break;
        }
        length += next;
    }
    return length;
}

/// `<FUZZY_SLOP: "~" ((<_NUM_CHAR>)+ (( "." (<_NUM_CHAR>)+ )? (<_TERM_CHAR>)*) | (<_TERM_CHAR>)*) >`
[[nodiscard]] size_t match_fuzzy_slop(const CodePoints &cps, size_t pos) noexcept {
    if (at_end(cps, pos) || at(cps, pos) != U'~') {
        return 0;
    }

    // First alternative: digits, an optional fractional part, then term chars.
    size_t numeric = 0;
    {
        size_t i = pos + 1;
        while (i < cps.size() && is_digit(at(cps, i))) {
            ++i;
        }
        if (i > pos + 1) {
            size_t after_fraction = i;
            if (i + 1 < cps.size() && at(cps, i) == U'.' && is_digit(at(cps, i + 1))) {
                size_t j = i + 1;
                while (j < cps.size() && is_digit(at(cps, j))) {
                    ++j;
                }
                after_fraction = j;
            }
            size_t j = after_fraction;
            while (true) {
                const size_t next = match_term_char(cps, j);
                if (next == 0) {
                    break;
                }
                j += next;
            }
            numeric = j - pos;
        }
    }

    // Second alternative: term chars only (this also matches a bare `~`).
    size_t plain = 1;
    while (true) {
        const size_t next = match_term_char(cps, pos + plain);
        if (next == 0) {
            break;
        }
        plain += next;
    }

    return std::max(numeric, plain);
}

/// `<PREFIXTERM: ("*") | ( <_TERM_START_CHAR> (<_TERM_CHAR>)* "*" ) >`
[[nodiscard]] size_t match_prefix_term(const CodePoints &cps, size_t pos) noexcept {
    if (!at_end(cps, pos) && at(cps, pos) == U'*') {
        return 1;
    }
    const size_t start = match_term_start_char(cps, pos);
    if (start == 0) {
        return 0;
    }
    size_t i = pos + start;
    while (true) {
        const size_t next = match_term_char(cps, i);
        if (next == 0) {
            break;
        }
        i += next;
    }
    if (i < cps.size() && at(cps, i) == U'*') {
        return i - pos + 1;
    }
    return 0;
}

/// `<WILDTERM: (<_TERM_START_CHAR> | [ "*", "?" ]) (<_TERM_CHAR> | [ "*", "?" ])* >`
[[nodiscard]] size_t match_wild_term(const CodePoints &cps, size_t pos) noexcept {
    size_t length = 0;
    if (const size_t start = match_term_start_char(cps, pos); start > 0) {
        length = start;
    } else if (!at_end(cps, pos) && (at(cps, pos) == U'*' || at(cps, pos) == U'?')) {
        length = 1;
    } else {
        return 0;
    }
    while (true) {
        const size_t next = match_term_char(cps, pos + length);
        if (next > 0) {
            length += next;
            continue;
        }
        if (!at_end(cps, pos + length) && (at(cps, pos + length) == U'*' || at(cps, pos + length) == U'?')) {
            length += 1;
            continue;
        }
        break;
    }
    return length;
}

/// `<REGEXPTERM: "/" (~[ "/" ] | "\\/")* "/" >`
[[nodiscard]] size_t match_regexp_term(const CodePoints &cps, size_t pos) noexcept {
    if (at_end(cps, pos) || at(cps, pos) != U'/') {
        return 0;
    }
    size_t i = pos + 1;
    while (i < cps.size()) {
        if (at(cps, i) == U'\\' && i + 1 < cps.size() && at(cps, i + 1) == U'/') {
            i += 2;
            continue;
        }
        if (at(cps, i) == U'/') {
            return i - pos + 1;
        }
        ++i;
    }
    return 0;
}

[[nodiscard]] size_t match_range_quoted(const CodePoints &cps, size_t pos) noexcept {
    if (at_end(cps, pos) || at(cps, pos) != U'"') {
        return 0;
    }
    size_t i = pos + 1;
    size_t content = 0;
    while (i < cps.size()) {
        if (at(cps, i) == U'\\' && i + 1 < cps.size() && at(cps, i + 1) == U'"') {
            i += 2;
            content += 1;
            continue;
        }
        if (at(cps, i) == U'"') {
            return content == 0 ? 0 : i - pos + 1;
        }
        ++i;
        content += 1;
    }
    return 0;
}

[[nodiscard]] size_t match_range_goop(const CodePoints &cps, size_t pos) noexcept {
    size_t length = 0;
    while (pos + length < cps.size()) {
        const char32_t value = at(cps, pos + length);
        if (value == U' ' || value == U']' || value == U'}') {
            break;
        }
        ++length;
    }
    return length;
}

[[nodiscard]] size_t match_number(const CodePoints &cps, size_t pos) noexcept {
    size_t i = pos;
    while (i < cps.size() && is_digit(at(cps, i))) {
        ++i;
    }
    if (i == pos) {
        return 0;
    }
    if (i + 1 < cps.size() && at(cps, i) == U'.' && is_digit(at(cps, i + 1))) {
        size_t j = i + 1;
        while (j < cps.size() && is_digit(at(cps, j))) {
            ++j;
        }
        i = j;
    }
    return i - pos;
}

enum class LexerState : uint8_t { kDefault, kRange, kBoost };

struct Rule {
    int32_t kind;
    size_t length;
    LexerState next_state;
};

/// Picks the longest match, the earliest rule winning a tie.
[[nodiscard]] Rule best_rule(const CodePoints &cps, size_t pos, LexerState state) {
    Rule best{query_syntax::EOF_TOKEN, 0, state};
    const auto consider = [&](int32_t kind, size_t length, LexerState next_state) {
        if (length > best.length) {
            best = Rule{kind, length, next_state};
        }
    };

    switch (state) {
        case LexerState::kDefault:
            consider(query_syntax::AND, match_and(cps, pos), LexerState::kDefault);
            consider(query_syntax::OR, match_or(cps, pos), LexerState::kDefault);
            consider(query_syntax::NOT, match_not(cps, pos), LexerState::kDefault);
            consider(query_syntax::PLUS, match_literal(cps, pos, U"+"), LexerState::kDefault);
            consider(query_syntax::MINUS, match_literal(cps, pos, U"-"), LexerState::kDefault);
            consider(query_syntax::BAREOPER, match_bare_oper(cps, pos), LexerState::kDefault);
            consider(query_syntax::LPAREN, match_literal(cps, pos, U"("), LexerState::kDefault);
            consider(query_syntax::RPAREN, match_literal(cps, pos, U")"), LexerState::kDefault);
            consider(query_syntax::COLON, match_literal(cps, pos, U":"), LexerState::kDefault);
            consider(query_syntax::STAR, match_literal(cps, pos, U"*"), LexerState::kDefault);
            consider(query_syntax::CARAT, match_literal(cps, pos, U"^"), LexerState::kBoost);
            consider(query_syntax::QUOTED, match_quoted(cps, pos), LexerState::kDefault);
            consider(query_syntax::TERM, match_term(cps, pos), LexerState::kDefault);
            consider(query_syntax::FUZZY_SLOP, match_fuzzy_slop(cps, pos), LexerState::kDefault);
            consider(query_syntax::PREFIXTERM, match_prefix_term(cps, pos), LexerState::kDefault);
            consider(query_syntax::WILDTERM, match_wild_term(cps, pos), LexerState::kDefault);
            consider(query_syntax::REGEXPTERM, match_regexp_term(cps, pos), LexerState::kDefault);
            consider(query_syntax::RANGEIN_START, match_literal(cps, pos, U"["), LexerState::kRange);
            consider(query_syntax::RANGEEX_START, match_literal(cps, pos, U"{"), LexerState::kRange);
            break;
        case LexerState::kRange:
            consider(query_syntax::RANGE_TO, match_literal(cps, pos, U"TO"), LexerState::kRange);
            consider(query_syntax::RANGEIN_END, match_literal(cps, pos, U"]"), LexerState::kDefault);
            consider(query_syntax::RANGEEX_END, match_literal(cps, pos, U"}"), LexerState::kDefault);
            consider(query_syntax::RANGE_QUOTED, match_range_quoted(cps, pos), LexerState::kRange);
            consider(query_syntax::RANGE_GOOP, match_range_goop(cps, pos), LexerState::kRange);
            break;
        case LexerState::kBoost:
            consider(query_syntax::NUMBER, match_number(cps, pos), LexerState::kDefault);
            break;
    }
    return best;
}

} // namespace

Result<std::vector<QueryToken>> tokenize_query(std::string_view input) {
    const CodePoints cps = decode_utf8(input);

    std::vector<QueryToken> tokens;
    LexerState state = LexerState::kDefault;
    size_t pos = 0;
    int32_t line = 1;
    int32_t column = 1;

    while (pos < cps.size()) {
        const Rule rule = best_rule(cps, pos, state);
        if (rule.length == 0) {
            // `<_WHITESPACE>` is skipped in the default and range states only.
            if ((state == LexerState::kDefault || state == LexerState::kRange) && is_whitespace(at(cps, pos))) {
                if (at(cps, pos) == U'\n') {
                    ++line;
                    column = 1;
                } else {
                    ++column;
                }
                ++pos;
                continue;
            }
            const std::string character(input.substr(cps[pos].offset, cps[pos].length));
            return err(LuceneError::parse("Encountered unexpected character \"" + character + "\" at line " +
                                          std::to_string(line) + ", column " + std::to_string(column) + "."));
        }

        const size_t begin_byte = cps[pos].offset;
        const size_t last = pos + rule.length - 1;
        const size_t end_byte = cps[last].offset + cps[last].length;

        QueryToken token;
        token.kind = rule.kind;
        token.image = std::string(input.substr(begin_byte, end_byte - begin_byte));
        token.begin_offset = begin_byte;
        token.end_offset = end_byte;
        token.begin_line = line;
        token.begin_column = column;
        tokens.push_back(std::move(token));

        for (size_t i = pos; i < pos + rule.length; ++i) {
            if (at(cps, i) == U'\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        pos += rule.length;
        state = rule.next_state;
    }

    QueryToken end;
    end.begin_offset = input.size();
    end.end_offset = input.size();
    end.begin_line = line;
    end.begin_column = column;
    tokens.push_back(std::move(end));
    return tokens;
}

Result<std::string> discard_escape_char(std::string_view image) {
    std::string output;
    output.reserve(image.size());

    bool last_was_escape = false;
    int32_t code_point_multiplier = 0;
    char32_t code_point = 0;

    const auto hex_value = [](char value) -> int32_t {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    };

    for (const char current : image) {
        if (code_point_multiplier > 0) {
            const int32_t digit = hex_value(current);
            if (digit < 0) {
                return err(LuceneError::parse(std::string("Non-hex character in Unicode escape sequence: ") +
                                              current));
            }
            code_point += static_cast<char32_t>(digit * code_point_multiplier);
            code_point_multiplier >>= 4;
            if (code_point_multiplier == 0) {
                append_utf8(output, code_point);
                code_point = 0;
            }
            continue;
        }

        if (last_was_escape) {
            if (current == 'u') {
                // The four hexadecimal digits of an escaped code point.
                code_point_multiplier = 16 * 16 * 16;
                last_was_escape = false;
                continue;
            }
            output.push_back(current);
            last_was_escape = false;
            continue;
        }

        if (current == '\\') {
            last_was_escape = true;
            continue;
        }
        output.push_back(current);
    }

    if (code_point_multiplier > 0) {
        return err(LuceneError::parse("Truncated Unicode escape sequence."));
    }
    if (last_was_escape) {
        return err(LuceneError::parse("Term can not end with escape character."));
    }
    return output;
}

} // namespace lucene
