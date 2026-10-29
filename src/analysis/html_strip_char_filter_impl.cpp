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

module lucene_core:html_strip_char_filter.impl;

import :html_strip_char_filter;

import :char_filter;
import :string_helper;
import :types;

import std;

namespace lucene {

namespace {

struct Entity {
    std::string_view name;
    char32_t code_point;
};

/// The named entities of the text this filter has to handle.  The table is scanned
/// linearly: it is only consulted when the input holds an `&`, and a page has far
/// fewer entities than text.
constexpr Entity kEntities[] = {
        {"AElig", 0x00C6},  {"Aacute", 0x00C1}, {"Acirc", 0x00C2},  {"Agrave", 0x00C0}, {"Aring", 0x00C5},
        {"Atilde", 0x00C3}, {"Auml", 0x00C4},   {"Ccedil", 0x00C7}, {"ETH", 0x00D0},    {"Eacute", 0x00C9},
        {"Ecirc", 0x00CA},  {"Egrave", 0x00C8}, {"Euml", 0x00CB},   {"Iacute", 0x00CD}, {"Icirc", 0x00CE},
        {"Igrave", 0x00CC}, {"Iuml", 0x00CF},   {"Ntilde", 0x00D1}, {"Oacute", 0x00D3}, {"Ocirc", 0x00D4},
        {"Ograve", 0x00D2}, {"Oslash", 0x00D8}, {"Otilde", 0x00D5}, {"Ouml", 0x00D6},   {"THORN", 0x00DE},
        {"Uacute", 0x00DA}, {"Ucirc", 0x00DB},  {"Ugrave", 0x00D9}, {"Uuml", 0x00DC},   {"Yacute", 0x00DD},
        {"aacute", 0x00E1}, {"acirc", 0x00E2},  {"acute", 0x00B4},  {"aelig", 0x00E6},  {"agrave", 0x00E0},
        {"amp", 0x0026},    {"aring", 0x00E5},  {"atilde", 0x00E3}, {"auml", 0x00E4},   {"brvbar", 0x00A6},
        {"ccedil", 0x00E7}, {"cedil", 0x00B8},  {"cent", 0x00A2},   {"copy", 0x00A9},   {"curren", 0x00A4},
        {"deg", 0x00B0},    {"divide", 0x00F7}, {"eacute", 0x00E9}, {"ecirc", 0x00EA},  {"egrave", 0x00E8},
        {"eth", 0x00F0},    {"euml", 0x00EB},   {"frac12", 0x00BD}, {"frac14", 0x00BC}, {"frac34", 0x00BE},
        {"gt", 0x003E},     {"iacute", 0x00ED}, {"icirc", 0x00EE},  {"iexcl", 0x00A1},  {"igrave", 0x00EC},
        {"iquest", 0x00BF}, {"iuml", 0x00EF},   {"laquo", 0x00AB},  {"lt", 0x003C},     {"macr", 0x00AF},
        {"micro", 0x00B5},  {"middot", 0x00B7}, {"nbsp", 0x00A0},   {"not", 0x00AC},    {"ntilde", 0x00F1},
        {"oacute", 0x00F3}, {"ocirc", 0x00F4},  {"ograve", 0x00F2}, {"ordf", 0x00AA},   {"ordm", 0x00BA},
        {"oslash", 0x00F8}, {"otilde", 0x00F5}, {"ouml", 0x00F6},   {"para", 0x00B6},   {"plusmn", 0x00B1},
        {"pound", 0x00A3},  {"quot", 0x0022},   {"raquo", 0x00BB},  {"reg", 0x00AE},    {"sect", 0x00A7},
        {"shy", 0x00AD},    {"sup1", 0x00B9},   {"sup2", 0x00B2},   {"sup3", 0x00B3},   {"szlig", 0x00DF},
        {"thorn", 0x00FE},  {"times", 0x00D7},  {"uacute", 0x00FA}, {"ucirc", 0x00FB},  {"ugrave", 0x00F9},
        {"uml", 0x00A8},    {"uuml", 0x00FC},   {"yacute", 0x00FD}, {"yen", 0x00A5},    {"yuml", 0x00FF},
        // A few punctuation entities that show up in real pages.
        {"apos", 0x0027},   {"bull", 0x2022},   {"dagger", 0x2020}, {"euro", 0x20AC},   {"hellip", 0x2026},
        {"ldquo", 0x201C},  {"lsquo", 0x2018},  {"mdash", 0x2014},  {"ndash", 0x2013},  {"permil", 0x2030},
        {"prime", 0x2032},  {"rdquo", 0x201D},  {"rsquo", 0x2019},  {"trade", 0x2122},  {"zwj", 0x200D},
        {"zwnj", 0x200C},
};

[[nodiscard]] std::optional<char32_t> lookup_entity(std::string_view name) {
    for (const Entity &entity : kEntities) {
        if (entity.name == name) {
            return entity.code_point;
        }
    }
    return std::nullopt;
}

/// The tag name introduced at `position` (which points at `<`), lower cased, and
/// where its `>` is; `nullopt` when the markup is not terminated.
struct Tag {
    std::string name;
    size_t end;
};

[[nodiscard]] std::optional<Tag> read_tag(std::string_view input, size_t position) {
    // `position` points at `<`.
    size_t cursor = position + 1;
    const bool closing = cursor < input.size() && input[cursor] == '/';
    if (closing) {
        ++cursor;
    }
    const size_t name_begin = cursor;
    while (cursor < input.size() && ((input[cursor] >= 'a' && input[cursor] <= 'z') ||
                                     (input[cursor] >= 'A' && input[cursor] <= 'Z') ||
                                     (input[cursor] >= '0' && input[cursor] <= '9'))) {
        ++cursor;
    }
    std::string name(input.substr(name_begin, cursor - name_begin));
    std::transform(name.begin(), name.end(), name.begin(), [](char value) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    });
    while (cursor < input.size() && input[cursor] != '>') {
        ++cursor;
    }
    if (cursor >= input.size()) {
        return std::nullopt;
    }
    return Tag{std::move(name), cursor + 1};
}

/// The content end of a `script`/`style` element that starts at `position`.
[[nodiscard]] std::optional<size_t> skip_raw_text_element(std::string_view input, std::string_view element,
                                                          size_t position) {
    const std::string closing = "</" + std::string(element);
    const size_t found = input.find(closing, position);
    if (found == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t close = input.find('>', found);
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    return close + 1;
}

} // namespace

Result<std::string> HTMLStripCharFilter::filter(std::string_view input) const {
    std::string output;
    output.reserve(input.size());

    size_t position = 0;
    while (position < input.size()) {
        const char current = input[position];

        if (current == '<') {
            auto tag = read_tag(input, position);
            if (!tag.has_value()) {
                // Not markup after all: keep the character.
                output.push_back(current);
                ++position;
                continue;
            }
            if (tag->name == "script" || tag->name == "style") {
                auto content_end = skip_raw_text_element(input, tag->name, tag->end);
                position = content_end.has_value() ? content_end.value() : tag->end;
                output.push_back(' ');
                continue;
            }
            // A tag becomes a space, so `<b>a</b><b>b</b>` does not become `ab`.
            position = tag->end;
            output.push_back(' ');
            continue;
        }

        if (current == '&') {
            const size_t semicolon = input.find(';', position);
            if (semicolon != std::string_view::npos && semicolon - position <= 10) {
                const std::string_view entity = input.substr(position + 1, semicolon - position - 1);
                std::optional<char32_t> decoded;
                if (!entity.empty() && entity.front() == '#') {
                    // Numeric entity: `&#233;` or `&#xE9;`.
                    const bool hexadecimal = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                    const std::string_view digits = hexadecimal ? entity.substr(2) : entity.substr(1);
                    if (!digits.empty() && digits.size() <= 8) {
                        char32_t value = 0;
                        bool valid = true;
                        for (const char digit : digits) {
                            int32_t digit_value = -1;
                            if (digit >= '0' && digit <= '9') {
                                digit_value = digit - '0';
                            } else if (hexadecimal && digit >= 'a' && digit <= 'f') {
                                digit_value = digit - 'a' + 10;
                            } else if (hexadecimal && digit >= 'A' && digit <= 'F') {
                                digit_value = digit - 'A' + 10;
                            }
                            if (digit_value < 0) {
                                valid = false;
                                break;
                            }
                            value = value * (hexadecimal ? 16 : 10) + static_cast<char32_t>(digit_value);
                        }
                        if (valid) {
                            decoded = value;
                        }
                    }
                } else {
                    decoded = lookup_entity(entity);
                }
                if (decoded.has_value()) {
                    if (!StringHelper::utf8_append(output, static_cast<int32_t>(decoded.value())).has_value()) {
                        // Not a valid code point: keep the entity as it is.
                        output.append(input.substr(position, semicolon - position + 1));
                    }
                    position = semicolon + 1;
                    continue;
                }
            }
        }

        output.push_back(current);
        ++position;
    }
    return output;
}

} // namespace lucene
