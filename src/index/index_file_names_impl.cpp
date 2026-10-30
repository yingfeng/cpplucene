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

module lucene_core:index_file_names.impl;

import :index_file_names;

import :error;
import :types;

import std;

namespace lucene {

std::string IndexFileNames::file_name(std::string_view base, std::string_view extension) {
    std::string name(base);
    if (!extension.empty()) {
        name += '.';
        name += extension;
    }
    return name;
}

std::string IndexFileNames::file_name_from_generation(std::string_view base, std::string_view extension,
                                                      int64_t generation) {
    if (generation < 0) {
        // Callers use -1 for "no generation"; keeping the plain name makes the
        // failure visible in the file listing instead of writing `base_-1`.
        return file_name(base, extension);
    }
    std::string name(base);
    if (base != PENDING_SEGMENTS) {
        name += '_';
        name += base36_encode(generation);
    }
    if (!extension.empty()) {
        name += '.';
        name += extension;
    }
    return name;
}

Result<int64_t> IndexFileNames::generation_from_segment_file_name(std::string_view name, std::string_view base) {
    if (name == OLD_SEGMENTS_GEN) {
        return err(LuceneError::illegal_argument("segments.gen no longer holds the generation"));
    }
    if (name == PENDING_SEGMENTS) {
        return err(LuceneError::illegal_argument("pending_segments has no generation"));
    }
    if (!name.starts_with(base) || name.size() <= base.size() || name[base.size()] != '_') {
        return err(LuceneError::illegal_argument("file name does not start with " + std::string(base) + "_ : " +
                                                 std::string(name)));
    }
    std::string_view generation = name.substr(base.size() + 1);
    const size_t dot = generation.find('.');
    if (dot != std::string_view::npos) {
        generation = generation.substr(0, dot);
    }
    return base36_decode(generation);
}

std::string IndexFileNames::strip_extension(std::string_view name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) {
        return std::string(name);
    }
    return std::string(name.substr(0, dot));
}

std::string IndexFileNames::strip_segment_name(std::string_view name, std::string_view base) {
    const std::string without_extension = strip_extension(name);
    if (without_extension == base) {
        return {};
    }
    if (without_extension.size() > base.size() + 1 && without_extension.starts_with(base) &&
        without_extension[base.size()] == '_') {
        return without_extension.substr(base.size() + 1);
    }
    return without_extension;
}

std::string IndexFileNames::strip_segment_name(std::string_view name) {
    size_t idx = name.find('_', 1);
    if (idx == std::string_view::npos) {
        idx = name.find('.');
    }
    if (idx == std::string_view::npos) {
        return std::string(name);
    }
    return std::string(name.substr(idx));
}

std::string IndexFileNames::base36_encode(int64_t value) {
    if (value == 0) {
        return "0";
    }
    // Negating INT64_MIN is undefined, so work on the magnitude in uint64.
    const bool negative = value < 0;
    uint64_t magnitude = negative ? (~static_cast<uint64_t>(value) + 1) : static_cast<uint64_t>(value);

    constexpr std::string_view kDigits = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string digits;
    while (magnitude != 0) {
        digits.push_back(kDigits[static_cast<size_t>(magnitude % 36)]);
        magnitude /= 36;
    }
    if (negative) {
        digits.push_back('-');
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

Result<int64_t> IndexFileNames::base36_decode(std::string_view text) {
    if (text.empty()) {
        return err(LuceneError::illegal_argument("empty base 36 number"));
    }
    size_t index = 0;
    const bool negative = text.front() == '-';
    if (negative) {
        index = 1;
        if (text.size() == 1) {
            return err(LuceneError::illegal_argument("base 36 number is only a sign: " + std::string(text)));
        }
    }
    uint64_t value = 0;
    for (; index < text.size(); ++index) {
        const char character = text[index];
        uint64_t digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<uint64_t>(character - '0');
        } else if (character >= 'a' && character <= 'z') {
            digit = static_cast<uint64_t>(character - 'a') + 10;
        } else if (character >= 'A' && character <= 'Z') {
            digit = static_cast<uint64_t>(character - 'A') + 10;
        } else {
            return err(LuceneError::illegal_argument("invalid base 36 digit '" + std::string(1, character) + "' in " +
                                                     std::string(text)));
        }
        if (value > (static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - digit) / 36) {
            return err(LuceneError::illegal_argument("base 36 number out of range: " + std::string(text)));
        }
        value = value * 36 + digit;
    }
    const int64_t magnitude = static_cast<int64_t>(value);
    return negative ? -magnitude : magnitude;
}

} // namespace lucene
