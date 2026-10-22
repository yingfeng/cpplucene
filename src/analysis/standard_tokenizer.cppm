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

export module lucene_core:standard_tokenizer;

import :types;
import :error;

import std;

import :tokenizer;

namespace lucene {

/// Word-break property classes of UAX #29 plus the Lucene-specific script
/// classes.
export enum class WordBreakClass : uint8_t {
    Other = 0,
    ALetter,
    Numeric,
    Katakana,
    Hiragana,
    Ideographic,
    Hangul,
    ExtendNumLet,
    MidLetter,
    MidNum,
    MidNumLet,
    Extend,
    Format,
    Zwj,
    RegionalIndicator,
    Emoji,
};

/// The `WordBreakProperty` value of a code point.
///
/// Whitespace and punctuation are `Other`.  Exported so that the unit tests can
/// assert the classification rules directly.
export [[nodiscard]] WordBreakClass word_break_class(char32_t codepoint) noexcept;

/// Token types produced by [`StandardTokenizer`] (`StandardTokenizer` constants
/// in Lucene / `TOKEN_TYPES` in `standard_tokenizer.rs`).
export struct StandardTokenizerTypes {
    static constexpr std::string_view ALPHANUM = "<ALPHANUM>";
    static constexpr std::string_view NUM = "<NUM>";
    static constexpr std::string_view SOUTHEAST_ASIAN = "<SOUTHEAST_ASIAN>";
    static constexpr std::string_view IDEOGRAPHIC = "<IDEOGRAPHIC>";
    static constexpr std::string_view HIRAGANA = "<HIRAGANA>";
    static constexpr std::string_view KATAKANA = "<KATAKANA>";
    static constexpr std::string_view HANGUL = "<HANGUL>";
    static constexpr std::string_view EMOJI = "<EMOJI>";
};

/// The Lucene standard tokenizer.
///
/// The reference implementation ports Lucene's JFlex-generated DFA (`ZZ_CMAP`/`ZZ_TRANS` tables);
/// here the UAX #29 word-boundary rules are implemented directly (WB3-WB16 plus
/// the script handling Lucene layers on top) because it is the maintainable
/// option: every rule below is traceable to the UAX #29 text, and the script
/// classes are explicit.
///
/// Behaviour that intentionally matches Lucene:
///  * Ideographic characters are emitted **one code point per token**.
///  * Hiragana, Katakana and Hangul runs are emitted as single tokens.
///  * `ALetter`/`Numeric` runs honour MidLetter/MidNum/MidNumLet (so `don't`,
///    `3.14` and `U.S.A` stay together) and `ExtendNumLet` (`_`).
///  * Emoji (including ZWJ sequences and skin-tone modifiers) form one token.
///  * Tokens longer than `max_token_length` code points are **skipped**, and the
///    skipped positions are added to the position increment of the next token
///    (Lucene "corrects" the offsets the same way; nothing is truncated).
export class StandardTokenizer : public Tokenizer {
public:
    /// `max_token_length` mirrors `StandardAnalyzer.DEFAULT_MAX_TOKEN_LENGTH`.
    explicit StandardTokenizer(size_t max_token_length = DEFAULT_MAX_TOKEN_LENGTH);

    [[nodiscard]] Result<bool> increment_token() override;

    /// Default maximum allowed token length.
    static constexpr size_t DEFAULT_MAX_TOKEN_LENGTH = 255;

    /// Absolute maximum size of a token (`MAX_TOKEN_LENGTH_LIMIT`).
    static constexpr size_t MAX_TOKEN_LENGTH_LIMIT = 1024UL * 1024UL;

    /// Sets the maximum token length; valid range is `[1, MAX_TOKEN_LENGTH_LIMIT]`.
    [[nodiscard]] VoidResult set_max_token_length(size_t length);

    [[nodiscard]] size_t max_token_length() const noexcept { return max_token_length_; }

private:
    /// Scans the token that starts at `start` (which must be a word-start code
    /// point) and fills `end` and `type`.
    void scan_token(size_t start, size_t &end, std::string_view &type) const noexcept;

    size_t max_token_length_;
};

} // namespace lucene
