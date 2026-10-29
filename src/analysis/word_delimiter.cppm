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

export module lucene_core:word_delimiter;

import :types;

import std;

namespace lucene {

/// A `BreakIterator`-like walk over the subwords [`WordDelimiterGraphFilter`](lucene_core:word_delimiter_graph_filter)
/// generates.
///
/// Port of `WordDelimiterIterator`
/// (`analysis/common/miscellaneous/word_delimiter_iterator.rs`).
///
/// **Deviation:** the reference implementation works on UTF-16 code units; this
/// port walks code points and reports **byte** offsets, because a term here is a
/// UTF-8 `std::string` and the offsets of the enclosing tokens are byte offsets
/// too.  `char_type` is therefore asked about code points, so a multi-byte
/// character is never split in the middle.
export class WordDelimiterIterator {
public:
    static constexpr int32_t kDone = -1;

    static constexpr int32_t kLower = 0x01;
    static constexpr int32_t kUpper = 0x02;
    static constexpr int32_t kDigit = 0x04;
    static constexpr int32_t kSubwordDelim = 0x08;

    /// `LOWER | UPPER`.
    static constexpr int32_t kAlpha = 0x03;
    /// `LOWER | UPPER | DIGIT`.
    static constexpr int32_t kAlphanum = 0x07;

    WordDelimiterIterator(std::string_view text, bool split_on_case_change, bool split_on_numerics,
                          bool stem_english_possessive);

    /// Restarts the walk over `text`.
    void set_text(std::string_view text);

    /// Advances to the next subword and returns its end byte offset, or [`kDone`].
    int32_t next();

    /// The type of the current subword: the type of its first character, with
    /// `LOWER`/`UPPER` collapsed into [`kAlpha`].
    [[nodiscard]] int32_t type() const;

    /// The byte offset where the current subword starts.
    [[nodiscard]] int32_t current() const;
    /// The byte offset where the current subword ends, or [`kDone`].
    [[nodiscard]] int32_t end() const;

    /// Whether the term is one subword, possibly surrounded by delimiters.
    [[nodiscard]] bool is_single_word() const;

    /// `Character.getType` reduced to this filter's four classes.
    [[nodiscard]] static int32_t get_type(char32_t codepoint) noexcept;

    [[nodiscard]] static bool is_alpha(int32_t type) noexcept { return (type & kAlpha) != 0; }
    [[nodiscard]] static bool is_digit(int32_t type) noexcept { return (type & kDigit) != 0; }
    [[nodiscard]] static bool is_subword_delim(int32_t type) noexcept { return (type & kSubwordDelim) != 0; }
    [[nodiscard]] static bool is_upper(int32_t type) noexcept { return (type & kUpper) != 0; }

private:
    [[nodiscard]] int32_t char_type(int32_t codepoint_index) const;
    void set_bounds();
    [[nodiscard]] bool is_break(int32_t last_type, int32_t type) const;
    [[nodiscard]] bool ends_with_possessive(int32_t codepoint_index) const;
    [[nodiscard]] int32_t byte_offset(int32_t codepoint_index) const;

    std::string text_{};
    /// `byte_offsets[i]` is the byte offset of code point `i`; the last entry is `text.size()`.
    std::vector<int32_t> byte_offsets_{};
    std::vector<int32_t> types_{};

    /// Start of the text, excluding leading delimiters (code point index).
    int32_t start_bounds_{0};
    /// End of the text, excluding trailing delimiters (code point index).
    int32_t end_bounds_{0};
    /// Beginning of the current subword (code point index).
    int32_t current_{0};
    /// End of the current subword (code point index), or [`kDone`].
    int32_t end_{0};

    bool has_final_possessive_{false};
    bool skip_possessive_{false};
    bool split_on_case_change_{true};
    bool split_on_numerics_{true};
    bool stem_english_possessive_{true};
};

} // namespace lucene
