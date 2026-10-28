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

export module lucene_core:strict_string_tokenizer;

import :error;
import :types;

import std;

namespace lucene {

/// Splits a string on a delimiter, keeping empty tokens and reporting an error
/// instead of an empty token once the input is spent.
///
/// Port of `util/strict_string_tokenizer.rs`; the version parser is its only
/// user.  The token views point into the string handed to the constructor,
/// which must therefore outlive the tokenizer.
export class StrictStringTokenizer {
public:
    StrictStringTokenizer(std::string_view text, char delimiter) : text_(text), delimiter_(delimiter) {}

    /// The next token, or `illegal_state` when there are none left.
    ///
    /// **Deviation:** the reference implementation returns a lifetime-carrying
    /// `&str`; here the view is into the string passed to the constructor.
    [[nodiscard]] Result<std::string_view> next_token();

    /// Whether a token may still be returned.
    [[nodiscard]] bool has_more_tokens() const noexcept { return pos_.has_value(); }

    /// The whole string this tokenizer walks.
    [[nodiscard]] std::string_view text() const noexcept { return text_; }

private:
    std::string_view text_;
    char delimiter_;
    std::optional<size_t> pos_{0};
};

} // namespace lucene
