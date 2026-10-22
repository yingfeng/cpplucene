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

export module lucene_core:standard_analyzer;

import :types;
import :error;

import std;

import :analyzer;
import :standard_tokenizer;

namespace lucene {

/// Filters [`StandardTokenizer`] with a [`LowerCaseFilter`] and a
/// [`StopFilter`].  Port of `analysis/standard/standard_analyzer.rs`.
///
/// The stop word set defaults to [`StopFilter::english_stop_words`].
export class StandardAnalyzer : public Analyzer {
public:
    StandardAnalyzer();
    explicit StandardAnalyzer(std::set<std::string, std::less<>> stop_words);

    [[nodiscard]] std::string to_string() const override { return "StandardAnalyzer"; }

    /// Sets the maximum token length; valid range is
    /// `[1, StandardTokenizer::MAX_TOKEN_LENGTH_LIMIT]`.
    [[nodiscard]] VoidResult set_max_token_length(size_t length);

    [[nodiscard]] size_t max_token_length() const noexcept { return max_token_length_; }

    [[nodiscard]] const std::set<std::string, std::less<>> &stop_words() const noexcept { return stop_words_; }

protected:
    [[nodiscard]] Result<std::unique_ptr<TokenStreamComponents>> create_components(
        std::string_view field_name) override;

private:
    size_t max_token_length_{StandardTokenizer::DEFAULT_MAX_TOKEN_LENGTH};
    std::set<std::string, std::less<>> stop_words_;
};

} // namespace lucene
