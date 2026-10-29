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

export module lucene_core:url_email_tokenizer;

import :attributes;
import :tokenizer;
import :types;

import std;

namespace lucene {

/// The token types of [`UrlEmailTokenizer`], also written to `TypeAttribute` as
/// `<ALPHANUM>`, `<NUM>`, `<URL>` and `<EMAIL>`.
export enum class UrlEmailTokenType : uint8_t {
    kAlphanum = 0,
    kNum = 1,
    kUrl = 2,
    kEmail = 3,
};

/// Tokenizes text with the UAX#29 word rules, additionally recognising URLs and
/// email addresses.
///
/// Port of `UAX29URLEmailTokenizer` (`analysis/common/email/`).  Words, numbers,
/// URLs and email addresses become one token each; a URL or an email address is
/// never split at its internal punctuation.
///
/// The reference implementation drives this from a JFlex DFA generated out of the
/// URI RFC grammars; this port scans the same grammar by hand (see the
/// implementation file for the deviations) and reuses
/// [`StandardTokenizer`](lucene_core:standard_tokenizer) for the word rules.
export class UrlEmailTokenizer final : public Tokenizer {
public:
    UrlEmailTokenizer();

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;

    /// The type of the token just returned by
    /// [`increment_token`](UrlEmailTokenizer::increment_token).
    [[nodiscard]] UrlEmailTokenType current_type() const noexcept { return current_type_; }

    /// The `TypeAttribute` value of `type`.
    [[nodiscard]] static std::string_view type_name(UrlEmailTokenType type) noexcept;

private:
    /// One pre-tokenized span of the input.
    struct Piece {
        size_t start{0};
        size_t end{0};
        UrlEmailTokenType type{UrlEmailTokenType::kAlphanum};
    };

    /// Runs the word rules over `[start, end)` and appends the tokens it finds.
    void append_word_pieces(std::string_view text, size_t start, size_t end);

    std::vector<Piece> pieces_{};
    size_t next_{0};
    UrlEmailTokenType current_type_{UrlEmailTokenType::kAlphanum};
    TypeAttribute *type_{nullptr};
};

} // namespace lucene
