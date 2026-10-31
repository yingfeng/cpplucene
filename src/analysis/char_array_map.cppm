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

export module lucene_core:char_array_map;

import :types;
import std;

import :unicode_util;

namespace lucene {

/// A map whose keys are compared as character arrays.
///
/// Port of `analysis/char_array_map.rs`.  Keys are code point strings
/// (`std::u32string`), which is what the reference's `Vec<char>` is; the string
/// overloads decode UTF-8 on the way in, and the case-insensitive variants fold
/// every code point with [`char_to_lower`].
///
/// **Deviation:** the reference relies on Rust's full Unicode
/// `char::to_lowercase`; here folding goes through the same script-range
/// approximation [`unicode_util`] uses for classification.
export template <typename T>
class CharArrayMap {
public:
    /// `ignore_case` requests case-insensitive keys.
    explicit CharArrayMap(bool ignore_case) : ignore_case_(ignore_case) {}

    // ---- insert ----

    /// Inserts `key`, returning the value it replaced.
    [[nodiscard]] std::optional<T> put(const std::u32string &key, T value) {
        std::u32string normalized = normalize(key);
        auto it = map_.find(normalized);
        if (it == map_.end()) {
            map_.emplace(std::move(normalized), std::move(value));
            return std::nullopt;
        }
        std::optional<T> previous = std::move(it->second);
        it->second = std::move(value);
        return previous;
    }

    /// Inserts `key`, which is decoded from UTF-8.
    [[nodiscard]] std::optional<T> put_str(std::string_view key, T value) { return put(decode(key), std::move(value)); }

    /// Inserts every `(key, value)` pair.
    void put_all_str(std::span<const std::pair<std::string, T>> entries) {
        for (const auto &[key, value] : entries) {
            (void)put_str(key, value);
        }
    }

    // ---- lookup ----

    /// The value of `key`, or null when absent.
    [[nodiscard]] const T *get(const std::u32string &key) const { return get(key, 0, key.size()); }

    /// The value of the `[offset, offset + length)` slice of `key`.
    [[nodiscard]] const T *get(const std::u32string &key, size_t offset, size_t length) const {
        if (map_.empty() || offset > key.size() || length > key.size() - offset) {
            return nullptr;
        }
        return get_normalized(normalize(std::u32string_view(key).substr(offset, length)));
    }

    /// The value of the UTF-8 `key`, or null when absent.
    ///
    /// The empty-map check comes first because the lookup runs once per token:
    /// without it every token of an analyzer with an empty stop set pays a UTF-8
    /// decode plus a hash of the decoded text to answer "no".
    [[nodiscard]] const T *get_str(std::string_view key) const {
        if (map_.empty()) {
            return nullptr;
        }
        return get(decode(key));
    }

    /// The value of the `[offset, offset + length)` slice of the UTF-8 `key`,
    /// where the offsets count **code points**, not bytes.
    [[nodiscard]] const T *get_str(std::string_view key, size_t offset, size_t length) const {
        return get(decode(key), offset, length);
    }

    /// Whether `key` is present.
    [[nodiscard]] bool contains_key(const std::u32string &key) const { return get(key) != nullptr; }
    [[nodiscard]] bool contains_key(const std::u32string &key, size_t offset, size_t length) const {
        return get(key, offset, length) != nullptr;
    }
    [[nodiscard]] bool contains_key_str(std::string_view key) const { return get_str(key) != nullptr; }
    [[nodiscard]] bool contains_key_str(std::string_view key, size_t offset, size_t length) const {
        return get_str(key, offset, length) != nullptr;
    }

    // ---- housekeeping ----

    void clear() noexcept { map_.clear(); }
    [[nodiscard]] size_t size() const noexcept { return map_.size(); }
    [[nodiscard]] bool is_empty() const noexcept { return map_.empty(); }
    [[nodiscard]] bool ignore_case() const noexcept { return ignore_case_; }

    /// The stored (already normalised) keys, in unspecified order.
    [[nodiscard]] std::vector<std::u32string> keys() const {
        std::vector<std::u32string> result;
        result.reserve(map_.size());
        for (const auto &entry : map_) {
            result.push_back(entry.first);
        }
        return result;
    }

    /// `{key=value, ...}`, using `format_value` for the values.
    template <typename F>
    [[nodiscard]] std::string to_string(F format_value) const {
        std::string text = "{";
        bool first = true;
        for (const auto &entry : map_) {
            if (!first) {
                text += ", ";
            }
            first = false;
            text += utf8(entry.first);
            text += '=';
            text += format_value(entry.second);
        }
        text += '}';
        return text;
    }

    // ---- code point helpers ----

    /// Decodes UTF-8 into code points, replacing malformed sequences with
    /// U+FFFD (the reference's `from_utf8_lossy` behaviour).
    [[nodiscard]] static std::u32string decode(std::string_view text) {
        std::u32string result;
        result.reserve(text.size());
        size_t i = 0;
        while (i < text.size()) {
            const auto lead = static_cast<unsigned char>(text[i]);
            size_t extra = 0;
            char32_t codepoint = 0;
            if (lead < 0x80) {
                result.push_back(static_cast<char32_t>(lead));
                i += 1;
                continue;
            }
            if ((lead & 0xE0U) == 0xC0U) {
                extra = 1;
                codepoint = lead & 0x1FU;
            } else if ((lead & 0xF0U) == 0xE0U) {
                extra = 2;
                codepoint = lead & 0x0FU;
            } else if ((lead & 0xF8U) == 0xF0U) {
                extra = 3;
                codepoint = lead & 0x07U;
            } else {
                result.push_back(0xFFFD);
                i += 1;
                continue;
            }
            if (i + extra >= text.size()) {
                result.push_back(0xFFFD);
                i += 1;
                continue;
            }
            bool valid = true;
            for (size_t k = 1; k <= extra; ++k) {
                const auto next = static_cast<unsigned char>(text[i + k]);
                if ((next & 0xC0U) != 0x80U) {
                    valid = false;
                    break;
                }
                codepoint = (codepoint << 6U) | (next & 0x3FU);
            }
            if (!valid) {
                result.push_back(0xFFFD);
                i += 1;
                continue;
            }
            result.push_back(codepoint);
            i += extra + 1;
        }
        return result;
    }

    /// Encodes code points back into UTF-8, replacing surrogates with U+FFFD.
    [[nodiscard]] static std::string utf8(std::u32string_view text) {
        std::string result;
        result.reserve(text.size());
        for (const char32_t codepoint : text) {
            if (codepoint < 0x80) {
                result.push_back(static_cast<char>(codepoint));
            } else if (codepoint < 0x800) {
                result.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
                result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
            } else if (codepoint < 0x10000) {
                if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
                    result += "\xEF\xBF\xBD";
                    continue;
                }
                result.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
                result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
                result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
            } else {
                result.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
                result.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
                result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
                result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
            }
        }
        return result;
    }

private:
    [[nodiscard]] const T *get_normalized(const std::u32string &normalized) const {
        auto it = map_.find(normalized);
        return it == map_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] std::u32string normalize(std::u32string_view key) const {
        if (!ignore_case_) {
            return std::u32string(key);
        }
        std::u32string folded(key);
        for (char32_t &codepoint : folded) {
            codepoint = char_to_lower(codepoint);
        }
        return folded;
    }

    bool ignore_case_;
    std::unordered_map<std::u32string, T> map_;
};

} // namespace lucene
