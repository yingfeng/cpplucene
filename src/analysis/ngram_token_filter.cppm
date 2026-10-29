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

export module lucene_core:ngram_token_filter;

import :token_stream;
import :types;

import std;

namespace lucene {

/// Replaces each term with all the n-grams of length `[min_gram, max_gram]` it
/// contains, in offset-then-length order.
///
/// Port of `NGramTokenFilter` (`analysis/common/ngram/ngram_token_filter.rs`).
/// The grams of one term share a position (the first keeps the term's position
/// increment, the rest get 0), and offsets are left alone.
export class NGramTokenFilter : public TokenFilter {
public:
    /// `min_gram` must be at least 1 and must not exceed `max_gram`; an invalid
    /// range is clamped, because a constructor cannot report an error.
    NGramTokenFilter(std::unique_ptr<TokenStream> input, int32_t min_gram, int32_t max_gram,
                     bool preserve_original = false);
    /// [`NGramTokenFilter`] for one gram size.
    NGramTokenFilter(std::unique_ptr<TokenStream> input, int32_t gram_size);

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;

private:
    int32_t min_gram_;
    int32_t max_gram_;
    bool preserve_original_;
    std::string current_;
    std::vector<size_t> offsets_;
    int32_t code_point_count_{0};
    int32_t gram_start_{0};
    int32_t gram_size_{0};
    int32_t position_increment_{0};
    bool active_{false};
};

/// Replaces each term with the n-grams of length `[min_gram, max_gram]` that
/// start at its first character.
///
/// Port of `EdgeNGramTokenFilter`.
export class EdgeNGramTokenFilter : public TokenFilter {
public:
    EdgeNGramTokenFilter(std::unique_ptr<TokenStream> input, int32_t min_gram, int32_t max_gram,
                         bool preserve_original = false);

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;

private:
    int32_t min_gram_;
    int32_t max_gram_;
    bool preserve_original_;
    std::string current_;
    std::vector<size_t> offsets_;
    int32_t code_point_count_{0};
    int32_t gram_size_{0};
    int32_t position_increment_{0};
    bool active_{false};
};

} // namespace lucene
