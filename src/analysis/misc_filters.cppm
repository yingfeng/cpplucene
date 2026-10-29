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

export module lucene_core:misc_filters;

import :token_stream;
import :types;

import std;

namespace lucene {

/// Keeps the tokens whose length (in code points) is inside `[min, max]`.
///
/// Port of `LengthFilter`
/// (`analysis/common/miscellaneous/length_filter.rs`).  A dropped token still
/// advances the position of the following one, like
/// [`StopFilter`](lucene_core:stop_filter).
export class LengthFilter final : public TokenFilter {
public:
    LengthFilter(std::unique_ptr<TokenStream> input, size_t min, size_t max);

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;

private:
    size_t min_;
    size_t max_;
    int32_t skipped_positions_{0};
};

/// Truncates every token to at most `length` code points.
///
/// Port of `TruncateTokenFilter`
/// (`analysis/common/miscellaneous/truncate_token_filter.rs`).
export class TruncateTokenFilter final : public TokenFilter {
public:
    TruncateTokenFilter(std::unique_ptr<TokenStream> input, size_t length);

    [[nodiscard]] Result<bool> increment_token() override;

private:
    size_t length_;
};

/// Emits at most `max_token_count` tokens.
///
/// Port of `LimitTokenCountFilter`
/// (`analysis/common/miscellaneous/limit_token_count_filter.rs`).
export class LimitTokenCountFilter final : public TokenFilter {
public:
    /// With `consume_all_tokens` the wrapped stream is drained after the limit is
    /// reached, so the caller still pays the analysis cost but sees the same
    /// tokens.
    LimitTokenCountFilter(std::unique_ptr<TokenStream> input, size_t max_token_count,
                          bool consume_all_tokens = false);

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;

private:
    size_t max_token_count_;
    bool consume_all_tokens_;
    size_t token_count_{0};
    bool exhausted_{false};
};

/// Removes an elided article from the front of a token: with the articles
/// `l`, `d` and `j`, `l'avion` becomes `avion`.
///
/// Port of `ElisionFilter`
/// (`analysis/common/miscellaneous/elision_filter.rs`).  The article is matched
/// case insensitively and must be followed by an apostrophe.
export class ElisionFilter final : public TokenFilter {
public:
    ElisionFilter(std::unique_ptr<TokenStream> input, std::set<std::string, std::less<>> articles);

    [[nodiscard]] Result<bool> increment_token() override;

    [[nodiscard]] const std::set<std::string, std::less<>> &articles() const noexcept { return articles_; }

    /// The French articles of the reference implementation.
    [[nodiscard]] static const std::set<std::string, std::less<>> &french_articles();

private:
    std::set<std::string, std::less<>> articles_;
};

/// Marks the tokens whose text is in `keywords` with the keyword attribute, so
/// that later filters (a stemmer) can skip them.
///
/// Port of `SetKeywordMarkerFilter`
/// (`analysis/common/miscellaneous/set_keyword_marker_filter.rs`).  Unlike
/// `KeywordMarkerFilter`, which only keeps an existing mark, this one sets it.
export class SetKeywordMarkerFilter final : public TokenFilter {
public:
    SetKeywordMarkerFilter(std::unique_ptr<TokenStream> input, std::set<std::string, std::less<>> keywords);

    [[nodiscard]] Result<bool> increment_token() override;

private:
    std::set<std::string, std::less<>> keywords_;
};

} // namespace lucene
