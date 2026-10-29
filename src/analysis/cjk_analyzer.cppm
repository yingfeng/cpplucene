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

export module lucene_core:cjk_analyzer;

import :analyzer;
import :token_stream;
import :types;

import std;

namespace lucene {

/// The CJK analysis chain: standard tokenization, width folding, case folding,
/// CJK bigrams and stop words.
///
/// Port of `CJKAnalyzer` (`analysis/common/cjk/cjk_analyzer.rs`):
/// `StandardTokenizer -> CJKWidthFilter -> LowerCaseFilter -> CJKBigramFilter ->
/// StopFilter`.  It indexes Chinese, Japanese and Korean text as overlapping
/// bigrams, which is what makes search over unsegmented text work without a
/// dictionary.
///
/// **Deviation:** the stop word set defaults to empty, like the reference
/// implementation, and the `outputUnigrams` variant of the bigram filter is not
/// ported (see [`CJKBigramFilter`](lucene_core:cjk_bigram_filter)).
export class CJKAnalyzer : public Analyzer {
public:
    CJKAnalyzer();
    explicit CJKAnalyzer(std::set<std::string, std::less<>> stop_words);

    [[nodiscard]] std::string to_string() const override { return "CJKAnalyzer"; }

protected:
    [[nodiscard]] Result<std::unique_ptr<TokenStreamComponents>> create_components(
        std::string_view field_name) override;

private:
    std::set<std::string, std::less<>> stop_words_;
};

} // namespace lucene
