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

export module lucene_core:tokenizer;

import :types;
import :error;

import std;

import :attribute_source;
import :attributes;
import :token_stream;
import :unicode_util;

namespace lucene {

/// Base class of every token stream that consumes raw text.
///
/// Port of `analysis/tokenizer.rs`.  The reader is a `std::string_view`
/// (rlucene's `ReaderEnum` only exists because Rust needs a type-erased reader;
/// `std::string_view` is the natural C++ currency and every caller in the
/// codecs has the whole field value in memory).
///
/// Offsets are **UTF-8 byte offsets** into the input, matching rlucene's
/// `usize` offsets (Java would use UTF-16 code units).
export class Tokenizer : public TokenStream {
public:
    Tokenizer();

    [[nodiscard]] AttributeSource &attributes() override { return attributes_; }

    /// Sets the text to tokenize; must be called before `reset()`.
    [[nodiscard]] virtual VoidResult set_reader(std::string_view input);

    [[nodiscard]] VoidResult reset() override;

    [[nodiscard]] const std::string &input() const noexcept { return input_; }

protected:
    /// Byte offset of the code point at `index` (`index == size()` gives the
    /// input length).
    [[nodiscard]] uint32_t byte_offset_at(size_t index) const noexcept;

    /// Number of code points in the input.
    [[nodiscard]] size_t code_point_count() const noexcept { return code_points_.size(); }

    [[nodiscard]] char32_t code_point_at(size_t index) const noexcept { return code_points_[index]; }

    /// The UTF-8 bytes of the code point range `[start, end)`.
    [[nodiscard]] std::string_view text_of(size_t start, size_t end) const noexcept;

    /// Fills term + offsets for the code point range and returns `true`.
    bool emit_token(size_t start, size_t end);

    AttributeSource attributes_;
    std::string input_;
    size_t position_{0};
    std::u32string code_points_{};
    std::vector<uint32_t> byte_offsets_{};
};

/// A tokenizer that splits on a per-character predicate.
///
/// Port of `CharTokenizer` (`analysis/util/char_tokenizer.rs`).
export class CharTokenizer : public Tokenizer {
public:
    [[nodiscard]] Result<bool> increment_token() override;

protected:
    /// `true` when the code point belongs to a token.
    [[nodiscard]] virtual bool is_token_char(char32_t codepoint) const = 0;
};

/// Splits on whitespace.
export class WhitespaceTokenizer : public CharTokenizer {
protected:
    [[nodiscard]] bool is_token_char(char32_t codepoint) const override { return !char_is_whitespace(codepoint); }
};

/// Splits on everything that is not a letter.
export class LetterTokenizer : public CharTokenizer {
protected:
    [[nodiscard]] bool is_token_char(char32_t codepoint) const override { return char_is_letter(codepoint); }
};

/// Emits the whole input as a single token (`KeywordTokenizer`).
export class KeywordTokenizer : public Tokenizer {
public:
    [[nodiscard]] Result<bool> increment_token() override;
};

} // namespace lucene
