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

export module lucene_core:english_analyzer;

import :analyzer;
import :token_stream;
import :types;

import std;

namespace lucene {

/// The English analysis chain: standard tokenization, possessive stripping, case
/// folding, stop words and Porter stemming.
///
/// Port of `EnglishAnalyzer` (`analysis/common/en/english_analyzer.rs`):
/// `StandardTokenizer -> EnglishPossessiveFilter -> LowerCaseFilter ->
/// StopFilter -> PorterStemFilter`.
///
/// A stem exclusion set marks its terms with a
/// [`SetKeywordMarkerFilter`](lucene_core:misc_filters), so they reach the index
/// exactly as they were written.
export class EnglishAnalyzer : public Analyzer {
public:
    /// English stop words (`StopFilter::english_stop_words`).
    EnglishAnalyzer();

    explicit EnglishAnalyzer(std::set<std::string, std::less<>> stop_words);

    /// The stop words plus a stem exclusion set.
    EnglishAnalyzer(std::set<std::string, std::less<>> stop_words, std::set<std::string, std::less<>> stem_exclusions);

    [[nodiscard]] std::string to_string() const override { return "EnglishAnalyzer"; }

protected:
    [[nodiscard]] Result<std::unique_ptr<TokenStreamComponents>> create_components(
        std::string_view field_name) override;

private:
    std::set<std::string, std::less<>> stop_words_;
    std::set<std::string, std::less<>> stem_exclusions_;
};

} // namespace lucene
