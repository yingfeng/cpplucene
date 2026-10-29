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

module lucene_core:mapping_char_filter.impl;

import :mapping_char_filter;

import :char_filter;
import :error;
import :string_helper;
import :types;

import std;

namespace lucene {

namespace {

/// The text between the `"` of `"…"`, decoding the backslash escapes a mapping
/// file may contain.
[[nodiscard]] Result<std::string> parse_quoted(std::string_view text, size_t &position) {
    if (position >= text.size() || text[position] != '"') {
        return err(LuceneError::parse("a mapping rule must start with a quote"));
    }
    ++position;
    std::string value;
    while (position < text.size()) {
        const char current = text[position++];
        if (current == '"') {
            return value;
        }
        if (current == '\\' && position < text.size()) {
            const char escaped = text[position++];
            switch (escaped) {
                case 'n':
                    value.push_back('\n');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 'u':
                    // \uXXXX: four hexadecimal digits.
                    if (position + 4 > text.size()) {
                        return err(LuceneError::parse("truncated \\u escape in a mapping rule"));
                    }
                    {
                        int32_t code_point = 0;
                        for (int32_t i = 0; i < 4; ++i) {
                            const char digit = text[position++];
                            int32_t value_of_digit = 0;
                            if (digit >= '0' && digit <= '9') {
                                value_of_digit = digit - '0';
                            } else if (digit >= 'a' && digit <= 'f') {
                                value_of_digit = digit - 'a' + 10;
                            } else if (digit >= 'A' && digit <= 'F') {
                                value_of_digit = digit - 'A' + 10;
                            } else {
                                return err(LuceneError::parse("non hexadecimal digit in a \\u escape"));
                            }
                            code_point = code_point * 16 + value_of_digit;
                        }
                        if (!StringHelper::utf8_append(value, code_point).has_value()) {
                            return err(LuceneError::parse("the \\u escape is not a valid code point"));
                        }
                    }
                    break;
                default:
                    value.push_back(escaped);
                    break;
            }
            continue;
        }
        value.push_back(current);
    }
    return err(LuceneError::parse("unterminated quote in a mapping rule"));
}

} // namespace

MappingCharFilter::MappingCharFilter(std::vector<std::pair<std::string, std::string>> mappings)
    : mappings_(std::move(mappings)) {
    // Longest input first, so a scan stops at the most specific rule.
    std::stable_sort(mappings_.begin(), mappings_.end(),
                     [](const auto &left, const auto &right) { return left.first.size() > right.first.size(); });
}

Result<MappingCharFilter> MappingCharFilter::parse(std::string_view rules_text) {
    std::vector<std::pair<std::string, std::string>> mappings;
    size_t position = 0;
    while (position < rules_text.size()) {
        // Skip the line break and any leading blanks.
        while (position < rules_text.size() && (rules_text[position] == '\n' || rules_text[position] == '\r' ||
                                                rules_text[position] == ' ' || rules_text[position] == '\t')) {
            ++position;
        }
        if (position >= rules_text.size()) {
            break;
        }
        if (rules_text[position] == '#') {
            while (position < rules_text.size() && rules_text[position] != '\n') {
                ++position;
            }
            continue;
        }

        auto from = parse_quoted(rules_text, position);
        if (!from.has_value()) {
            return std::unexpected(std::move(from.error()));
        }
        while (position < rules_text.size() && (rules_text[position] == ' ' || rules_text[position] == '\t')) {
            ++position;
        }
        if (position + 1 >= rules_text.size() || rules_text[position] != '=' || rules_text[position + 1] != '>') {
            return err(LuceneError::parse("a mapping rule needs `=>` between the two strings"));
        }
        position += 2;
        while (position < rules_text.size() && (rules_text[position] == ' ' || rules_text[position] == '\t')) {
            ++position;
        }
        auto to = parse_quoted(rules_text, position);
        if (!to.has_value()) {
            return std::unexpected(std::move(to.error()));
        }
        mappings.emplace_back(std::move(from.value()), std::move(to.value()));
    }
    return MappingCharFilter(std::move(mappings));
}

Result<std::string> MappingCharFilter::filter(std::string_view input) const {
    std::string output;
    output.reserve(input.size());

    size_t position = 0;
    while (position < input.size()) {
        bool matched = false;
        for (const auto &rule : mappings_) {
            if (rule.first.empty() || rule.first.size() > input.size() - position) {
                continue;
            }
            if (input.compare(position, rule.first.size(), rule.first) == 0) {
                output += rule.second;
                position += rule.first.size();
                matched = true;
                break;
            }
        }
        if (!matched) {
            output.push_back(input[position]);
            ++position;
        }
    }
    return output;
}

std::string MappingCharFilter::to_string() const {
    return "MappingCharFilter(" + std::to_string(mappings_.size()) + " rules)";
}

} // namespace lucene
